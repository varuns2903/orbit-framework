#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/orm/Model.hpp>
#include <orbit/http/json.hpp>
#include <iostream>

using namespace server;
using namespace http;
using namespace database;

// 1. Define the model
struct User {
    int id = 0; // 0 for auto-increment usually
    std::string username;
    int age;
    bool is_active;
};

// 2. Tell the JSON library how to serialize it
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(User, id, username, age, is_active)

// 3. Register the ORM Model
ORBIT_REGISTER_MODEL(User, "users")

int main() {
    config::ServerConfig config;
    config.port = 8082;
    App app(config);

    app.get("/users", [](HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
        auto pg_client = std::make_shared<PostgresClient>(&writer->proactor(), "dbname=postgres user=postgres");
        
        // C++20 Coroutine lambda
        auto coro = [writer, pg_client]() -> concurrency::Task {
            bool connected = co_await connect_async(pg_client);
            if (!connected) {
                HttpResponse out;
                out.status(HttpStatus::InternalServerError).send("DB Connection Failed");
                writer->send(std::move(out));
                co_return;
            }

            // Create table (Raw query)
            co_await query_async(pg_client, "CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, username VARCHAR(50), age INT, is_active BOOLEAN);");

            // ORM INSERT
            User new_user{0, "john_doe", 28, true};
            co_await query_User(pg_client).insert_async(new_user);

            // ORM SELECT with WHERE clause
            std::vector<User> active_users = co_await query_User(pg_client)
                                                .where("is_active", "=", "true")
                                                .get_async();

            nlohmann::json response = active_users;
            
            HttpResponse out;
            out.status(HttpStatus::OK).send(response.dump());
            out.headers["Content-Type"] = "application/json";
            writer->send(std::move(out));
        };
        
        coro(); // Execute
    });

    std::cout << "Starting ORM Server on port 8082...\n";
    std::cout << "Test: curl http://localhost:8082/users\n";
    app.listen();

    return 0;
}
