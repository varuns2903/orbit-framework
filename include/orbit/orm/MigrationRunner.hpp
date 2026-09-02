#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <orbit/database/ResultSet.hpp>
#include <orbit/concurrency/Task.hpp>



namespace orm {

/**
 * @brief Simple Database Migration Runner for C++ Coroutines
 * 
 * Scans a directory for .sql files, checks which have been applied 
 * against the `orbit_migrations` table, and applies any pending files sequentially.
 */
template <typename DbClient>
class MigrationRunner {
public:
    static concurrency::Task run_migrations(std::shared_ptr<DbClient> db, const std::string& migrations_dir, std::shared_ptr<http::ResponseWriter> res) {
        // 1. Create tracking table
        co_await database::query_async(db, 
            "CREATE TABLE IF NOT EXISTS orbit_migrations ("
            "id SERIAL PRIMARY KEY, "
            "version VARCHAR(255) UNIQUE NOT NULL, "
            "applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);"
        );

        // 2. Fetch applied migrations
        database::ResultSet applied_res = co_await database::query_async(db, 
            "SELECT version FROM orbit_migrations ORDER BY version ASC;"
        );

        std::vector<std::string> applied_versions;
        for (size_t i = 0; i < applied_res.size(); ++i) {
            applied_versions.push_back(applied_res[i].get(0).value_or(""));
        }

        // 3. Scan directory
        if (!std::filesystem::exists(migrations_dir)) {
            std::cout << "[Migrations] Directory '" << migrations_dir << "' not found. Skipping migrations.\n";
            res->send(http::HttpResponse().status(http::HttpStatus::OK).send("Migrations skipped - no directory"));
            co_return;
        }

        std::vector<std::string> pending_files;
        for (const auto& entry : std::filesystem::directory_iterator(migrations_dir)) {
            if (entry.path().extension() == ".sql") {
                pending_files.push_back(entry.path().string());
            }
        }
        std::sort(pending_files.begin(), pending_files.end());

        // 4. Apply pending migrations
        int executed = 0;
        for (const auto& filepath : pending_files) {
            std::string filename = std::filesystem::path(filepath).filename().string();
            
            if (std::find(applied_versions.begin(), applied_versions.end(), filename) == applied_versions.end()) {
                std::cout << "[Migrations] Applying " << filename << "..." << std::endl;
                
                std::ifstream ifs(filepath);
                if (!ifs.is_open()) {
                    std::cerr << "[Migrations] Failed to open " << filepath << std::endl;
                    continue;
                }
                
                std::string sql((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
                
                // Run the migration SQL
                co_await database::query_async(db, sql);

                // Record it in the tracking table
                std::string insert_tracking = "INSERT INTO orbit_migrations (version) VALUES ('" + filename + "');";
                co_await database::query_async(db, insert_tracking);
                
                executed++;
            }
        }

        if (executed == 0) {
            res->send(http::HttpResponse().status(http::HttpStatus::OK).send("Database is up to date"));
        } else {
            res->send(http::HttpResponse().status(http::HttpStatus::OK).send("Successfully applied " + std::to_string(executed) + " migrations."));
        }
    }
};

} // namespace orm
