#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <memory>
#include <orbit/database/ResultSet.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
// Migrations use PostgreSQL (advisory locks, transactional DDL).
#ifdef ORBIT_ENABLE_POSTGRES
#include <orbit/database/PostgresCoro.hpp>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <orbit/network/KqueueProactor.hpp>
#elif defined(_WIN32)
#include <orbit/network/IocpProactor.hpp>
#else
#include <orbit/network/EpollProactor.hpp>
#endif
#else
#error "orm::MigrationRunner needs PostgreSQL support, which this build of Orbit leaves out (ORBIT_ENABLE_POSTGRES=OFF)"
#endif



namespace orbit::orm {

/**
 * @brief Database migration runner (PostgreSQL)
 * 
 * Scans a directory for .sql files, checks which have been applied 
 * against the `orbit_migrations` table, and applies any pending files in
 * name order. Each file runs in its own transaction together with its
 * tracking row, so a failing migration leaves no trace and stops the run.
 * A PostgreSQL advisory lock keeps concurrent runners (e.g. several app
 * instances starting at once) from applying the same file twice.
 *
 * Statements that cannot run inside a transaction (such as
 * CREATE INDEX CONCURRENTLY) are not supported.
 */
/// The outcome of a migration run.
struct MigrationResult {
    std::vector<std::string> applied; ///< Files applied by this run, in order.
    std::string error;                ///< Why the run stopped; empty on success.
    bool directory_missing = false;   ///< The migrations directory does not exist (not an error).

    bool ok() const { return error.empty(); }

    /// "Database is up to date", "Successfully applied N migrations.",
    /// "Migrations skipped - no directory" or the error.
    std::string summary() const {
        if (!ok()) return error;
        if (directory_missing) return "Migrations skipped - no directory";
        if (applied.empty()) return "Database is up to date";
        return "Successfully applied " + std::to_string(applied.size()) + " migrations.";
    }
};

template <typename DbClient>
class MigrationRunner {
public:
    static constexpr long long kAdvisoryLockKey = 0x6f72626974; // "orbit"

    /**
     * @brief Applies the pending migrations in @p migrations_dir over a
     *        connected client, with no HTTP involved: for start-up code, a
     *        `migrate` command or tests.
     *
     * A missing directory is not an error (nothing to apply).
     */
    // Parameters are taken by value: a coroutine outlives its caller's temporaries.
    static concurrency::Awaitable<MigrationResult> run(std::shared_ptr<DbClient> db, std::string migrations_dir) {
        MigrationResult result;
        // Awaiters are named locals: GCC 13 hits an internal compiler error on
        // co_await expressions holding non-trivial temporaries.
        auto lock_query = database::query_async(db, "SELECT pg_advisory_lock(" + std::to_string(kAdvisoryLockKey) + ");");
        auto lock = co_await lock_query;
        if (!lock.ok()) {
            result.error = "Could not take the migration lock: " + lock.error();
            co_return result;
        }

        do {
            auto create_query = database::query_async(db,
                "CREATE TABLE IF NOT EXISTS orbit_migrations ("
                "id SERIAL PRIMARY KEY, "
                "version VARCHAR(255) UNIQUE NOT NULL, "
                "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);");
            auto created = co_await create_query;
            if (!created.ok()) { result.error = "Could not create orbit_migrations: " + created.error(); break; }

            auto applied_query = database::query_async(db, "SELECT version FROM orbit_migrations ORDER BY version ASC;");
            database::ResultSet applied_res = co_await applied_query;
            if (!applied_res.ok()) { result.error = "Could not read orbit_migrations: " + applied_res.error(); break; }

            std::vector<std::string> applied_versions;
            for (size_t i = 0; i < applied_res.size(); ++i) {
                applied_versions.push_back(applied_res[i].get(0).value_or(""));
            }

            if (!std::filesystem::exists(migrations_dir)) {
                LOG_INFO("[Migrations] Directory '" << migrations_dir << "' not found. Skipping migrations.");
                result.directory_missing = true;
                break;
            }

            std::vector<std::string> pending_files;
            for (const auto& entry : std::filesystem::directory_iterator(migrations_dir)) {
                if (entry.path().extension() == ".sql") {
                    pending_files.push_back(entry.path().string());
                }
            }
            std::sort(pending_files.begin(), pending_files.end());

            for (const auto& filepath : pending_files) {
                std::string filename = std::filesystem::path(filepath).filename().string();
                if (std::find(applied_versions.begin(), applied_versions.end(), filename) != applied_versions.end()) {
                    continue;
                }

                std::ifstream ifs(filepath);
                if (!ifs.is_open()) { result.error = "Failed to open " + filepath; break; }
                std::string sql((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

                LOG_INFO("[Migrations] Applying " << filename << "...");
                auto begin_query = database::query_async(db, "BEGIN;");
                auto begin = co_await begin_query;
                if (!begin.ok()) { result.error = "BEGIN failed: " + begin.error(); break; }

                auto run_query = database::query_async(db, sql);
                auto ran = co_await run_query;
                database::ResultSet track;
                if (ran.ok()) {
                    std::vector<std::optional<std::string>> tracking_params;
                    tracking_params.emplace_back(filename);
                    auto track_query = database::query_async(db, "INSERT INTO orbit_migrations (version) VALUES ($1);", tracking_params);
                    track = co_await track_query;
                }
                if (!ran.ok() || !track.ok()) {
                    auto rollback_query = database::query_async(db, "ROLLBACK;");
                    co_await rollback_query;
                    result.error = filename + " failed and was rolled back: " + (ran.ok() ? track.error() : ran.error());
                    break;
                }

                auto commit_query = database::query_async(db, "COMMIT;");
                auto commit = co_await commit_query;
                if (!commit.ok()) { result.error = "COMMIT of " + filename + " failed: " + commit.error(); break; }
                result.applied.push_back(filename);
            }
        } while (false);

        auto unlock_query = database::query_async(db, "SELECT pg_advisory_unlock(" + std::to_string(kAdvisoryLockKey) + ");");
        co_await unlock_query;
        if (!result.ok()) LOG_ERROR("[Migrations] " << result.error);
        co_return result;
    }

    /**
     * @brief run(), answered over HTTP: 200 with the summary, or a 500 whose
     *        details go to the log only.
     */
    static concurrency::Task run_migrations(std::shared_ptr<DbClient> db, std::string migrations_dir, std::shared_ptr<http::ResponseWriter> res) {
        auto running = run(std::move(db), std::move(migrations_dir));
        MigrationResult result = co_await running;
        if (result.ok()) {
            res->send(http::HttpResponse().status(http::HttpStatus::OK).send(result.summary()));
        } else {
            res->send(http::HttpResponse().status(http::HttpStatus::InternalServerError).send("Migrations failed; see server log"));
        }
    }
};

/**
 * @brief Connects to PostgreSQL and applies the pending migrations in
 *        @p migrations_dir, blocking until done: for start-up, before
 *        App::listen(), or a separate `migrate` command.
 *
 * Runs its own short-lived event loop on the calling thread, so it needs no
 * App or server. Gives up after @p timeout with an error result.
 */
inline MigrationResult migrate_sync(const std::string& conninfo, const std::string& migrations_dir,
                                    std::chrono::milliseconds timeout = std::chrono::minutes(5)) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    network::KqueueProactor proactor;
#elif defined(_WIN32)
    network::IocpProactor proactor;
#else
    network::EpollProactor proactor;
#endif
    auto db = std::make_shared<database::PostgresClient>(&proactor, conninfo);
    auto done = std::make_shared<std::optional<MigrationResult>>();
    [](std::shared_ptr<database::PostgresClient> client, std::string dir,
       std::shared_ptr<std::optional<MigrationResult>> out) -> concurrency::Task {
        auto connecting = database::connect_async(client);
        if (!co_await connecting) {
            MigrationResult failed;
            failed.error = "Could not connect to the database";
            *out = std::move(failed);
            co_return;
        }
        auto running = MigrationRunner<database::PostgresClient>::run(client, std::move(dir));
        *out = co_await running;
    }(db, migrations_dir, done);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!*done && std::chrono::steady_clock::now() < deadline) proactor.run_once(50);
    if (*done) return std::move(**done);
    // The coroutine stays suspended on an operation this loop will never
    // complete; it is abandoned with the loop.
    MigrationResult timed_out;
    timed_out.error = "Migrations did not finish within the timeout";
    return timed_out;
}

} // namespace orm
