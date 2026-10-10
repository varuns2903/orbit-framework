#include <gtest/gtest.h>
#include <orbit/server/App.hpp>

#include <stdexcept>
#include <string>

// App::listen() refuses to start if routes were registered more than once
// (#201). Does not reference HttpMethod::DELETE by name: App.hpp pulls in
// windows.h on Windows, whose DELETE macro clashes with the enumerator
// (see test_router_paths.cpp).

using namespace orbit::http;
using orbit::routing::RouteHandler;

namespace {

RouteHandler noop_handler() {
    return [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("ok")); };
}

} // namespace

TEST(AppRouteValidationTest, NoDuplicatesValidatesClean) {
    orbit::server::App app(orbit::config::ServerConfig{});
    app.get("/a", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("a")); });
    app.post("/a", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("a-post")); });
    EXPECT_TRUE(app.validate_routes().empty());
}

TEST(AppRouteValidationTest, ExactDuplicateStaticRouteIsReported) {
    orbit::server::App app(orbit::config::ServerConfig{});
    app.get("/dup", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("1")); });
    app.get("/dup", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("2")); });
    auto problems = app.validate_routes();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0], "GET /dup registered 2 times");
}

TEST(AppRouteValidationTest, ExactDuplicateDynamicRouteIsReported) {
    orbit::server::App app(orbit::config::ServerConfig{});
    app.get("/tasks/:id", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("1")); });
    app.get("/tasks/:id", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("2")); });
    auto problems = app.validate_routes();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0], "GET /tasks/:id registered 2 times");
}

// Different patterns that could overlap at request time (:id vs {id:int})
// are not flagged: only an exact, identical pattern registered twice is.
TEST(AppRouteValidationTest, DifferentPatternsAreNotFlaggedAsDuplicates) {
    orbit::server::App app(orbit::config::ServerConfig{});
    app.get("/tasks/:id", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("a")); });
    app.get("/tasks/{id:int}", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("b")); });
    EXPECT_TRUE(app.validate_routes().empty());
}

TEST(AppRouteValidationTest, GroupRegisteredDuplicatesAreReported) {
    orbit::server::App app(orbit::config::ServerConfig{});
    app.group("/api", [](orbit::routing::Router& r) {
        r.get("/x", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("1")); });
        r.get("/x", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("2")); });
    });
    auto problems = app.validate_routes();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0], "GET /api/x registered 2 times");
}

TEST(AppRouteValidationTest, ListenThrowsWithoutBindingAnySocket) {
    orbit::config::ServerConfig cfg;
    cfg.port = 0; // would bind an ephemeral port if it got that far
    orbit::server::App app(cfg);
    app.get("/dup", noop_handler());
    app.get("/dup", noop_handler());
    EXPECT_THROW(app.listen(), std::invalid_argument);
}

TEST(AppRouteValidationTest, UnknownSegmentTypeThrowsAtRegistration) {
    orbit::server::App app(orbit::config::ServerConfig{});
    EXPECT_THROW(app.get("/tasks/{id:weird}", noop_handler()), std::invalid_argument);
    EXPECT_THROW(app.get("/tasks/{}", noop_handler()), std::invalid_argument);
}
