#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/orm/Model.hpp>
#include <orbit/http/json.hpp>
#include <iostream>

using namespace orbit::server;
using namespace orbit::http;
using namespace orbit::database;

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
    orbit::config::ServerConfig config;
    config.port = 8082;
    App app(config);

    app.get("/users", [](HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
        auto pg_client = std::make_shared<PostgresClient>(&writer->proactor(), "dbname=postgres user=postgres");
        
        // C++20 Coroutine lambda
        // The writer is a parameter, not a capture: that is how an exception in
        // the coroutine reaches the error handler (see docs/database.md).
        auto coro = [pg_client](std::shared_ptr<ResponseWriter> res_writer) -> orbit::concurrency::Task {
            bool connected = co_await connect_async(pg_client);
            if (!connected) {
                HttpResponse out;
                out.status(HttpStatus::InternalServerError).send("DB Connection Failed");
                res_writer->send(std::move(out));
                co_return;
            }

            // Create table (Raw query)
            co_await query_async(pg_client, "CREATE TABLE IF NOT EXISTS users (id SERIAL PRIMARY KEY, username VARCHAR(50), age INT, is_active BOOLEAN);");

            // ORM INSERT: id 0 is "unset", so the database generates it, and
            // create_async() returns the stored row with the new id.
            User new_user{0, "john_doe", 28, true};
            auto create = query_User(pg_client).create_async(new_user);
            User created = co_await create;
            std::cout << "Created user " << created.id << "\n";

            // ORM SELECT with Expression Templates (C++ DSL)
            std::vector<User> active_users = co_await query_User(pg_client)
                                                .where(orbit::orm::Col("is_active") == true && orbit::orm::Col("age") > 18)
                                                .get_async();

            nlohmann::json response = active_users;
            
            HttpResponse out;
            out.status(HttpStatus::OK).send(response.dump());
            out.headers["Content-Type"] = "application/json";
            res_writer->send(std::move(out));
        };
        
        coro(writer); // Execute
    });

    std::cout << "Starting ORM Server on port 8082...\n";
    std::cout << "Test: curl http://localhost:8082/users\n";
    app.listen();

    return 0;
}
