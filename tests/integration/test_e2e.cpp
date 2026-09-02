#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/middleware/Proxy.hpp>
#include "../utils/TestClient.hpp"

#include <thread>
#include <chrono>
#include <memory>

using namespace orbit;
using namespace http;

class E2EServerTest : public ::testing::Test {
protected:
    static server::App* app_ptr;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg;
        cfg.port = 8089; // Use unique port
        app_ptr = new server::App(cfg);

        // 1. Global Middlewares
        app_ptr->use(middleware::cors());
        app_ptr->use(middleware::compress());
        app_ptr->use(middleware::rate_limit(1000, std::chrono::seconds(60)));
        
        // 2. Simple GET
        app_ptr->get("/api/hello", [](const HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
            HttpResponse res;
            res.body = "Hello World";
            writer->send(std::move(res));
        });

        // 3. Dynamic route
        app_ptr->get("/api/users/:id", [](const HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
            HttpResponse res;
            res.body = "User: " + req.params.at("id");
            writer->send(std::move(res));
        });

        // 4. JSON body parser & Magic return
        app_ptr->post("/api/echo", [](const HttpRequest& req) -> nlohmann::json {
            auto j = req.json();
            j["echoed"] = true;
            return j;
        });

        // 5. JWT Auth group
        app_ptr->group("/secure", [](routing::Router& r) {
            r.use(middleware::jwt_auth("secret_key"));
            r.get("/data", [](const HttpRequest& /*req*/, std::shared_ptr<ResponseWriter> writer) {
                HttpResponse res;
                res.body = "Secure Data";
                writer->send(std::move(res));
            });
        });

        // Start server in background
        server_thread = std::thread([]() {
            app_ptr->listen();
        });
        
        // Wait for server to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        if (app_ptr) {
            app_ptr->stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
            delete app_ptr;
            app_ptr = nullptr;
        }
    }
};

server::App* E2EServerTest::app_ptr = nullptr;
std::thread E2EServerTest::server_thread;

TEST_F(E2EServerTest, SimpleGetRequest) {
    auto res = test::send_request("GET", "http://127.0.0.1:8089/api/hello");
    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, "Hello World");
    EXPECT_TRUE(res.headers.count("Access-Control-Allow-Origin") > 0);
}

TEST_F(E2EServerTest, DynamicRouteParsing) {
    auto res = test::send_request("GET", "http://127.0.0.1:8089/api/users/99");
    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body, "User: 99");
}

TEST_F(E2EServerTest, JsonPostRequest) {
    std::string payload = "{\"key\":\"value\"}";
    auto res = test::send_request("POST", "http://127.0.0.1:8089/api/echo", payload, {"Content-Type: application/json"});
    EXPECT_EQ(res.status_code, 200);
    EXPECT_NE(res.body.find("\"echoed\":true"), std::string::npos);
}

TEST_F(E2EServerTest, JwtAuthBlocksUnauthorized) {
    auto res = test::send_request("GET", "http://127.0.0.1:8089/secure/data");
    EXPECT_EQ(res.status_code, 401); // Middleware now properly executes and rejects unauthorized requests
}

TEST_F(E2EServerTest, NonExistentRoute) {
    auto res = test::send_request("GET", "http://127.0.0.1:8089/404/not/found");
    EXPECT_EQ(res.status_code, 404);
}
