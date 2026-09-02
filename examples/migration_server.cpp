#include <orbit/server/App.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/MigrationRunner.hpp>
#include <orbit/concurrency/Task.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>

using namespace server;
using namespace http;
using namespace database;
using namespace orm;

int main(int argc, char* argv[]) {
    auto config = config::ServerConfig::parse(argc, argv);
    App app(config);

    // Create a dummy migration directory and file for testing
    std::filesystem::create_directory("migrations");
    std::ofstream("migrations/001_create_users.sql") << "CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, username VARCHAR(255));\n";

    // Run migrations on startup!
    app.get("/migrate", [](HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> res) {
        auto coro = [res]() -> concurrency::Task {
            auto db = std::make_shared<PostgresClient>(&res->proactor(), "dbname=postgres");
            bool connected = co_await connect_async(db);
            
            if (connected) {
                MigrationRunner<PostgresClient>::run_migrations(db, "migrations", res);
            } else {
                res->send(HttpResponse().status(HttpStatus::InternalServerError).send("DB Connection Failed"));
            }
        };
        coro();
    });

    app.listen();
    return 0;
}
