#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestClient.hpp"
#include "../utils/TestConfig.hpp"

#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// The App-level registration API (every verb, with and without route
// middleware), on_error, health checks, and the TLS and shutdown calls
// that need no running server.

using namespace http;

namespace {

constexpr int kPort = 8150;

std::string url(const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(kPort) + path;
}

routing::RouteHandler reply(std::string body) {
    return [body](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.set_body(body, "text/plain");
        w->send(std::move(res));
    };
}

// Route middleware that marks the response, so the test can see it ran.
routing::Middleware mark(std::string value) {
    return [value](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        w->set_header("X-Route-Mw", value);
        return true;
    };
}

} // namespace

class AppApiTest : public ::testing::Test {
protected:
    static std::unique_ptr<server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<server::App>(cfg);

        app->put("/verb", reply("put"))
            .patch("/verb", reply("patch"))
            .del("/verb", reply("delete"))
            .options("/verb", reply("options"));
        app->get("/mw", {mark("get")}, reply("get"))
            .post("/mw", {mark("post")}, reply("post"))
            .put("/mw", {mark("put")}, reply("put"))
            .patch("/mw", {mark("patch")}, reply("patch"))
            .del("/mw", {mark("delete")}, reply("delete"))
            .options("/mw", {mark("options")}, reply("options"));

        app->on_error([](const std::exception& e, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.status(HttpStatus::BadRequest).set_body(std::string("app handler: ") + e.what(), "text/plain");
            w->send(std::move(res));
        });
        app->get("/throws", [](HttpRequest&, std::shared_ptr<ResponseWriter>) {
            throw std::invalid_argument("bad input");
        });

        app->enable_health_checks();
        app->enable_health_checks("/live", "/ready");

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            if (orbit::test::send_request("GET", url("/healthz")).status_code == 200) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
    }
};

std::unique_ptr<server::App> AppApiTest::app;
std::thread AppApiTest::server_thread;

TEST_F(AppApiTest, EveryVerbIsRouted) {
    for (const char* m : {"PUT", "PATCH", "DELETE", "OPTIONS"}) {
        auto res = orbit::test::send_request(m, url("/verb"));
        EXPECT_EQ(res.status_code, 200) << m;
        std::string expect = m;
        for (auto& c : expect) c = static_cast<char>(std::tolower(c));
        EXPECT_EQ(res.body, expect) << m;
    }
}

TEST_F(AppApiTest, EveryVerbRunsItsRouteMiddleware) {
    for (const char* m : {"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}) {
        auto res = orbit::test::send_request(m, url("/mw"));
        std::string expect = m;
        for (auto& c : expect) c = static_cast<char>(std::tolower(c));
        EXPECT_EQ(res.status_code, 200) << m;
        EXPECT_EQ(res.body, expect) << m;
        EXPECT_EQ(res.headers["X-Route-Mw"], expect) << m;
    }
}

TEST_F(AppApiTest, OnErrorHandlesHandlerExceptions) {
    auto res = orbit::test::send_request("GET", url("/throws"));
    EXPECT_EQ(res.status_code, 400);
    EXPECT_EQ(res.body, "app handler: bad input");
}

TEST_F(AppApiTest, HealthChecksAnswerOnDefaultAndCustomPaths) {
    auto live = orbit::test::send_request("GET", url("/healthz"));
    EXPECT_EQ(live.status_code, 200);
    EXPECT_EQ(live.body, "ok");
    auto ready = orbit::test::send_request("GET", url("/readyz"));
    EXPECT_EQ(ready.status_code, 200);
    EXPECT_EQ(ready.body, "ready");

    EXPECT_EQ(orbit::test::send_request("GET", url("/live")).body, "ok");
    EXPECT_EQ(orbit::test::send_request("GET", url("/ready")).body, "ready");
    EXPECT_FALSE(app->is_draining());
}

TEST(AppControlTest, ReloadTlsWithoutTlsFails) {
    config::ServerConfig cfg = orbit::test::server_config();
    cfg.port = kPort + 1;
    server::App app(cfg);
    std::string error;
    EXPECT_FALSE(app.reload_tls(&error));
    EXPECT_EQ(error, "TLS is not enabled");
    EXPECT_FALSE(app.reload_tls());
}

// A shutdown requested before listen() still applies: listen() returns at
// once, and the app reports itself draining.
TEST(AppControlTest, ShutdownBeforeListenStopsIt) {
    config::ServerConfig cfg = orbit::test::server_config();
    cfg.host = "127.0.0.1";
    cfg.port = kPort + 2;
    server::App app(cfg);
    app.shutdown(std::chrono::seconds(1));
    EXPECT_TRUE(app.is_draining());

    auto listening = std::async(std::launch::async, [&app] { app.listen(); });
    ASSERT_EQ(listening.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "listen() kept running after shutdown()";
}
