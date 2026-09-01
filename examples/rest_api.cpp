#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/middleware/Validation.hpp>
#include <orbit/http/json.hpp>
#include <iostream>
#include <memory>
#include <string>

using namespace server;
using namespace http;
using namespace middleware;
using namespace database;
using namespace concurrency;

// C++20 Coroutine Handler Example
Task coro_db_handler(HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer, std::shared_ptr<PostgresClient> pg_client) {
    // 1. Await database connection natively without blocking threads!
    bool connected = co_await connect_async(pg_client);
    if (!connected) {
        HttpResponse res;
        res.status(HttpStatus::InternalServerError).send("DB Connection Failed");
        writer->send(std::move(res));
        co_return; // Important: co_return exits the coroutine
    }

    // 2. Await query
    ResultSet res = co_await query_async(pg_client, "SELECT current_timestamp;");
    if (!res.empty()) {
        std::string ts = res[0].get(0).value_or("");
        
        HttpResponse out;
        out.status(HttpStatus::OK).send("{\"timestamp\": \"" + ts + "\"}");
        out.headers["Content-Type"] = "application/json";
        writer->send(std::move(out));
    } else {
        HttpResponse out;
        out.status(HttpStatus::InternalServerError).send("Query failed");
        writer->send(std::move(out));
    }
}

int main() {
    config::ServerConfig config;
    config.port = 8081;

    App app(config);

    // FEATURE 1: Global Error Handling Middleware
    app.on_error([](const std::exception& e, HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
        std::cerr << "[Global Error Handler] Caught exception: " << e.what() << "\n";
        HttpResponse res;
        nlohmann::json err;
        err["error"] = "Internal Server Error";
        err["message"] = e.what();
        res.status(HttpStatus::InternalServerError).send(err.dump());
        res.headers["Content-Type"] = "application/json";
        writer->send(std::move(res));
    });

    // FEATURE 2: Automated JSON Schema Validation
    std::vector<SchemaField> user_schema = {
        {"username", JsonType::STRING, true},
        {"age", JsonType::NUMBER, true},
        {"is_active", JsonType::BOOLEAN, false}
    };

    app.post("/users", {validate_json(user_schema)}, [](HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
        // We know for a fact json() has "username" and "age" correctly typed!
        nlohmann::json j = req.json();
        std::string username = j["username"];
        int age = j["age"];
        (void)age;

        HttpResponse res;
        res.status(HttpStatus::Created).send("{\"status\": \"user created\", \"user\": \"" + username + "\"}");
        res.headers["Content-Type"] = "application/json";
        writer->send(std::move(res));
    });

    // Throw route to test global error handler
    app.get("/crash", [](HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> /*writer*/) {
        throw std::runtime_error("Simulated catastrophic failure in route handler!");
    });

    // FEATURE 3: C++20 Coroutines
    // Note: To test this locally, you need a postgres server on localhost.
    app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
        auto pg_client = std::make_shared<PostgresClient>(&writer->proactor(), "dbname=postgres user=postgres");
        coro_db_handler(req, writer, pg_client);
    });

    std::cout << "REST API Server starting on port 8081...\n";
    std::cout << "Test validation: curl -X POST -d '{\"username\":\"john\",\"age\":30}' http://localhost:8081/users\n";
    std::cout << "Test validation failure: curl -X POST -d '{\"age\":\"string_not_number\"}' http://localhost:8081/users\n";
    std::cout << "Test error handler: curl http://localhost:8081/crash\n";
    
    app.listen();

    return 0;
}
