#include <gtest/gtest.h>
#include <orbit/orbit.hpp>

#include <memory>
#include <stdexcept>
#include <type_traits>

// <orbit/orbit.hpp> (#211): one include for a typical app, plus short
// aliases (orbit::App, orbit::Request, ...) for the namespaced types.
// Does not reference HttpMethod::DELETE by name: App.hpp pulls in
// windows.h on Windows, whose DELETE macro clashes with the enumerator
// (see test_router_paths.cpp).

namespace {

class UmbrellaMockWriter : public orbit::http::ResponseWriter {
public:
    orbit::Response last;
    void send(orbit::Response&& r) override { last = std::move(r); }
    void send_headers(orbit::Response&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(orbit::Response&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

} // namespace

// Each alias is the same type as its namespaced spelling, not merely
// something convertible to it.
TEST(OrbitUmbrellaTest, AliasesAreTheirNamespacedTypes) {
    static_assert(std::is_same_v<orbit::App, orbit::server::App>);
    static_assert(std::is_same_v<orbit::Request, orbit::http::HttpRequest>);
    static_assert(std::is_same_v<orbit::Response, orbit::http::HttpResponse>);
    static_assert(std::is_same_v<orbit::Status, orbit::http::HttpStatus>);
    static_assert(std::is_same_v<orbit::Method, orbit::http::HttpMethod>);
    static_assert(std::is_same_v<orbit::Writer, std::shared_ptr<orbit::http::ResponseWriter>>);
    static_assert(std::is_same_v<orbit::RouteHandler, orbit::routing::RouteHandler>);
    static_assert(std::is_same_v<orbit::Middleware, orbit::routing::Middleware>);
    static_assert(std::is_same_v<orbit::json, nlohmann::json>);
    SUCCEED();
}

// An app written entirely with the short aliases, as the issue's example
// shows, builds, routes, and answers through a real (mock) writer.
TEST(OrbitUmbrellaTest, ShortAliasesBuildARealHandler) {
    orbit::RouteHandler handler = [](orbit::Request& req, orbit::Writer w) {
        orbit::json body = {{"path", req.uri}};
        w->send(orbit::Response().json(body, orbit::Status::Created));
    };

    orbit::Request req;
    req.method = orbit::Method::GET;
    req.uri = "/hello";
    auto writer = std::make_shared<UmbrellaMockWriter>();
    handler(req, writer);

    EXPECT_EQ(writer->last.status_code, orbit::Status::Created);
    EXPECT_EQ(nlohmann::json::parse(writer->last.body)["path"], "/hello");
}

TEST(OrbitUmbrellaTest, AppConstructsThroughTheAlias) {
    orbit::App app(orbit::config::ServerConfig{});
    app.get("/ping", [](orbit::Request&, orbit::Writer w) { w->send(orbit::Response().send("pong")); });
    SUCCEED(); // registering the route and constructing App via the alias did not throw
}
