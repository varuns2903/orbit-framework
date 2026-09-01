#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/database/RedisClient.hpp>
#include <iostream>

using namespace server;
using namespace http;

int main(int argc, char* argv[]) {
    auto config = config::ServerConfig::parse(argc, argv);
    App app(config);

    // Redis example — RedisClient is synchronous, so we use it directly in handlers
    app.get("/redis/ping", [](HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
#ifdef ORBIT_ENABLE_REDIS
        database::RedisClient redis("127.0.0.1", 6379);
        if (!redis.connect()) {
            HttpResponse res;
            res.status(HttpStatus::InternalServerError);
            res.json(nlohmann::json{{"error", "Redis connection failed"}});
            writer->send(std::move(res));
            return;
        }

        auto pong = redis.ping();
        HttpResponse res;
        res.json(nlohmann::json{{"status", "ok"}, {"ping", pong}});
        writer->send(std::move(res));
#else
        HttpResponse res;
        res.json(nlohmann::json{{"error", "Redis disabled at compile time"}});
        writer->send(std::move(res));
#endif
    });

    app.get("/redis/test", [](HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
#ifdef ORBIT_ENABLE_REDIS
        database::RedisClient redis("127.0.0.1", 6379);
        if (!redis.connect()) {
            HttpResponse res;
            res.status(HttpStatus::InternalServerError);
            res.json(nlohmann::json{{"error", "Redis connection failed"}});
            writer->send(std::move(res));
            return;
        }

        redis.set("orbit_test", "hello_orbit", 60);
        auto val = redis.get("orbit_test");
        HttpResponse res;
        res.json(nlohmann::json{
            {"status", "ok"},
            {"key", "orbit_test"},
            {"value", val.value_or("(nil)")}
        });
        writer->send(std::move(res));
#else
        HttpResponse res;
        res.json(nlohmann::json{{"error", "Redis disabled at compile time"}});
        writer->send(std::move(res));
#endif
    });

    std::cout << "Database server running on http://localhost:" << config.port << "\n";
    std::cout << "Test endpoints: /redis/ping, /redis/test\n";
    app.listen();
    return 0;
}
