#include <orbit/server/App.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/RedisClient.hpp>
#include <iostream>

using namespace server;
using namespace http;
using namespace database;

int main() {
    App app;
    auto& proactor = app.get_event_loop().get_proactor();

    app.get("/postgres", [&proactor](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
#ifdef ORBIT_ENABLE_POSTGRES
        auto pg = std::make_shared<PostgresClient>(&proactor, "host=localhost user=postgres password=postgres dbname=test");
        pg->connect([pg, res](bool success) {
            if (!success) {
                HttpResponse response;
                response.status_code = HttpStatus::INTERNAL_SERVER_ERROR;
                response.json(std::string("{\"error\": \"DB Connection Failed\"}"));
                res->send(std::move(response));
                return;
            }
            pg->query("SELECT 1 AS test", [pg, res](pg_result* db_res) {
                HttpResponse response;
                response.json(std::string("{\"status\": \"Postgres Query Success\"}"));
                res->send(std::move(response));
            });
        });
#else
        HttpResponse response;
        response.json(std::string("{\"error\": \"Postgres disabled at compile time\"}"));
        res->send(std::move(response));
#endif
    });

    app.get("/redis", [&proactor](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
#ifdef ORBIT_ENABLE_REDIS
        auto redis = std::make_shared<RedisClient>(&proactor, "127.0.0.1", 6379);
        redis->connect([redis, res](bool success) {
            if (!success) {
                HttpResponse response;
                response.status_code = HttpStatus::INTERNAL_SERVER_ERROR;
                response.json(std::string("{\"error\": \"Redis Connection Failed\"}"));
                res->send(std::move(response));
                return;
            }
            redis->set("test_key", "orbit_rocks", [redis, res](bool) {
                redis->get("test_key", [redis, res](std::optional<std::string> val) {
                    HttpResponse response;
                    if (val) {
                        response.json(std::string("{\"status\": \"Redis Success\", \"value\": \"") + *val + "\"}");
                    } else {
                        response.json(std::string("{\"error\": \"Redis key not found\"}"));
                    }
                    res->send(std::move(response));
                });
            });
        });
#else
        HttpResponse response;
        response.json(std::string("{\"error\": \"Redis disabled at compile time\"}"));
        res->send(std::move(response));
#endif
    });

    std::cout << "Database server running on http://localhost:8080\n";
    std::cout << "Test endpoints: /postgres, /redis\n";
    app.start();
    return 0;
}
