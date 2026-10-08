#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <orbit/database/ResultSet.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>



namespace orbit::orm {

/**
 * @brief Simple Database Migration Runner for C++ Coroutines (PostgreSQL)
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
template <typename DbClient>
class MigrationRunner {
public:
    static constexpr long long kAdvisoryLockKey = 0x6f72626974; // "orbit"

    // Parameters are taken by value: a coroutine outlives its caller's temporaries.
    static concurrency::Task run_migrations(std::shared_ptr<DbClient> db, std::string migrations_dir, std::shared_ptr<http::ResponseWriter> res) {
        auto fail = [res](const std::string& message) {
            LOG_ERROR("[Migrations] " << message);
            res->send(http::HttpResponse().status(http::HttpStatus::InternalServerError).send("Migrations failed; see server log"));
        };

        auto lock = co_await database::query_async(db, "SELECT pg_advisory_lock(" + std::to_string(kAdvisoryLockKey) + ");");
        if (!lock.ok()) {
            fail("Could not take the migration lock: " + lock.error());
            co_return;
        }

        // "error:<details>" on failure, otherwise the message for the client.
        std::string outcome;
        do {
            auto created = co_await database::query_async(db, 
                "CREATE TABLE IF NOT EXISTS orbit_migrations ("
                "id SERIAL PRIMARY KEY, "
                "version VARCHAR(255) UNIQUE NOT NULL, "
                "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);"
            );
            if (!created.ok()) { outcome = "error:Could not create orbit_migrations: " + created.error(); break; }

            database::ResultSet applied_res = co_await database::query_async(db, 
                "SELECT version FROM orbit_migrations ORDER BY version ASC;"
            );
            if (!applied_res.ok()) { outcome = "error:Could not read orbit_migrations: " + applied_res.error(); break; }

            std::vector<std::string> applied_versions;
            for (size_t i = 0; i < applied_res.size(); ++i) {
                applied_versions.push_back(applied_res[i].get(0).value_or(""));
            }

            if (!std::filesystem::exists(migrations_dir)) {
                LOG_INFO("[Migrations] Directory '" << migrations_dir << "' not found. Skipping migrations.");
                outcome = "Migrations skipped - no directory";
                break;
            }

            std::vector<std::string> pending_files;
            for (const auto& entry : std::filesystem::directory_iterator(migrations_dir)) {
                if (entry.path().extension() == ".sql") {
                    pending_files.push_back(entry.path().string());
                }
            }
            std::sort(pending_files.begin(), pending_files.end());

            int executed = 0;
            for (const auto& filepath : pending_files) {
                std::string filename = std::filesystem::path(filepath).filename().string();
                if (std::find(applied_versions.begin(), applied_versions.end(), filename) != applied_versions.end()) {
                    continue;
                }

                std::ifstream ifs(filepath);
                if (!ifs.is_open()) { outcome = "error:Failed to open " + filepath; break; }
                std::string sql((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

                LOG_INFO("[Migrations] Applying " << filename << "...");
                auto begin = co_await database::query_async(db, "BEGIN;");
                if (!begin.ok()) { outcome = "error:BEGIN failed: " + begin.error(); break; }

                auto run = co_await database::query_async(db, sql);
                database::ResultSet track = run;
                if (run.ok()) {
                    // A named vector, not a braced list: GCC 13 crashes on braced
                    // initializer lists inside co_await expressions.
                    std::vector<std::optional<std::string>> tracking_params;
                    tracking_params.emplace_back(filename);
                    track = co_await database::query_async(db, "INSERT INTO orbit_migrations (version) VALUES ($1);", tracking_params);
                }
                if (!run.ok() || !track.ok()) {
                    co_await database::query_async(db, "ROLLBACK;");
                    outcome = "error:" + filename + " failed and was rolled back: " + (run.ok() ? track.error() : run.error());
                    break;
                }

                auto commit = co_await database::query_async(db, "COMMIT;");
                if (!commit.ok()) { outcome = "error:COMMIT of " + filename + " failed: " + commit.error(); break; }
                executed++;
            }
            if (!outcome.empty()) break;

            outcome = executed == 0 ? "Database is up to date"
                                    : "Successfully applied " + std::to_string(executed) + " migrations.";
        } while (false);

        co_await database::query_async(db, "SELECT pg_advisory_unlock(" + std::to_string(kAdvisoryLockKey) + ");");

        if (outcome.rfind("error:", 0) == 0) {
            fail(outcome.substr(6));
        } else {
            res->send(http::HttpResponse().status(http::HttpStatus::OK).send(outcome));
        }
    }
};

} // namespace orm
