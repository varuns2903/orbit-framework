#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <csignal>
#include <future>
#include <string>
#include <thread>

namespace {

bool can_connect(uint16_t port) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    network::close_socket(fd);
    return ok;
}

void wait_until_listening(uint16_t port) {
    for (int i = 0; i < 200 && !can_connect(port); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Runs app.listen() on a thread; returns a future that is ready when it returns.
std::future<void> run(server::App& app) {
    return std::async(std::launch::async, [&app] { app.listen(); });
}

// A lambda, not a function: route handlers are deduced from operator().
const auto noop = [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
    http::HttpResponse res;
    res.set_body("ok");
    w->send(std::move(res));
};

} // namespace

TEST(AppLifecycleTest, EachAppPublishesItsOwnOpenApiSpec) {
    config::ServerConfig cfg;
    server::App a(cfg);
    server::App b(cfg);
    a.get("/only-in-a", noop);
    b.get("/only-in-b", noop);
    a.openapi().register_schema("SchemaA", "{\"type\":\"object\"}");
    // Registered the old way: shared by every App, as before.
    openapi::OpenApiRegistry::instance().register_schema("SharedSchema", "{\"type\":\"string\"}");

    std::string spec_a = a.openapi().generate_swagger_json("A", "1");
    std::string spec_b = b.openapi().generate_swagger_json("B", "1");

    EXPECT_NE(spec_a.find("/only-in-a"), std::string::npos);
    EXPECT_EQ(spec_a.find("/only-in-b"), std::string::npos);
    EXPECT_NE(spec_b.find("/only-in-b"), std::string::npos);
    EXPECT_EQ(spec_b.find("/only-in-a"), std::string::npos);

    EXPECT_NE(spec_a.find("SchemaA"), std::string::npos);
    EXPECT_EQ(spec_b.find("SchemaA"), std::string::npos);
    EXPECT_NE(spec_a.find("SharedSchema"), std::string::npos);
    EXPECT_NE(spec_b.find("SharedSchema"), std::string::npos);
}

TEST(AppLifecycleTest, GroupsShareTheirAppsRegistry) {
    config::ServerConfig cfg;
    server::App app(cfg);
    app.group("/api", [](routing::Router& api) {
        api.group("/v1", [](routing::Router& v1) { v1.get("/items/:id", noop); });
    });
    EXPECT_NE(app.openapi().generate_swagger_json("T", "1").find("/api/v1/items/{id}"), std::string::npos);
}

TEST(AppLifecycleTest, StopBeforeTheLoopExistsStillStopsIt) {
    config::ServerConfig cfg;
    cfg.port = 8118;
    server::App app(cfg);
    app.stop(); // before listen(): must not be lost
    auto done = run(app);
    EXPECT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "listen() kept running after an earlier stop()";
}

TEST(AppLifecycleTest, StopRightAfterStartingIsSafe) {
    // stop() races with listen() creating the event loop; under TSan this
    // used to report a data race, and a lost stop could hang the test.
    for (int i = 0; i < 20; ++i) {
        config::ServerConfig cfg;
        cfg.port = 8119;
        server::App app(cfg);
        auto done = run(app);
        app.stop();
        ASSERT_EQ(done.wait_for(std::chrono::seconds(5)), std::future_status::ready) << "iteration " << i;
    }
}

#ifndef _WIN32
TEST(AppLifecycleTest, SignalStopsEveryApp) {
    config::ServerConfig cfg_a;
    cfg_a.port = 8120;
    config::ServerConfig cfg_b;
    cfg_b.port = 8121;
    server::App a(cfg_a);
    server::App b(cfg_b);
    auto done_a = run(a);
    auto done_b = run(b);
    wait_until_listening(8120);
    wait_until_listening(8121);

    // Both Apps installed the handler; one SIGTERM must reach both.
    std::raise(SIGTERM);

    EXPECT_EQ(done_a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(done_b.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    // Clean up whichever did not stop, so a failure does not hang the binary.
    a.stop();
    b.stop();
}
#endif
