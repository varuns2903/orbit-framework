#include <gtest/gtest.h>
#include <orbit/middleware/Cors.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <map>
#include <memory>
#include <stdexcept>

using namespace http;

namespace {

class CorsTestWriter : public ResponseWriter {
public:
    std::map<std::string, std::string> default_headers;
    HttpResponse last_response;
    bool sent = false;

    void send(HttpResponse&& response) override {
        last_response = std::move(response);
        sent = true;
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string& key, const std::string& value) override { default_headers[key] = value; }
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

middleware::CorsOptions allow_list() {
    middleware::CorsOptions o;
    o.allowed_origins = {"https://app.example"};
    o.allow_credentials = true;
    return o;
}

} // namespace

TEST(CorsTest, DisallowedOriginGetsNoAllowOrigin) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers["Origin"] = "https://evil.example";
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_TRUE(m(req, w));
    EXPECT_EQ(w->default_headers.count("Access-Control-Allow-Origin"), 0u);
    EXPECT_EQ(w->default_headers.count("Access-Control-Allow-Credentials"), 0u);
    EXPECT_EQ(w->default_headers["Vary"], "Origin");
}

TEST(CorsTest, AllowedOriginIsReflectedWithCredentials) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers["Origin"] = "https://app.example";
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_TRUE(m(req, w));
    EXPECT_EQ(w->default_headers["Access-Control-Allow-Origin"], "https://app.example");
    EXPECT_EQ(w->default_headers["Access-Control-Allow-Credentials"], "true");
    EXPECT_EQ(w->default_headers["Vary"], "Origin");
}

TEST(CorsTest, PreflightFromDisallowedOriginHasNoCorsHeaders) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::OPTIONS;
    req.headers["Origin"] = "https://evil.example";
    req.headers["Access-Control-Request-Method"] = "DELETE";
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_FALSE(m(req, w));
    ASSERT_TRUE(w->sent);
    EXPECT_EQ(w->last_response.headers.count("Access-Control-Allow-Methods"), 0u);
    EXPECT_EQ(w->default_headers.count("Access-Control-Allow-Origin"), 0u);
}

TEST(CorsTest, PreflightFromAllowedOriginListsMethods) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::OPTIONS;
    req.headers["Origin"] = "https://app.example";
    req.headers["Access-Control-Request-Method"] = "DELETE";
    req.headers["Access-Control-Request-Headers"] = "X-Custom";
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_FALSE(m(req, w));
    EXPECT_EQ(w->last_response.status_code, HttpStatus::NoContent);
    EXPECT_NE(w->last_response.headers["Access-Control-Allow-Methods"].find("DELETE"), std::string::npos);
    // "*" is literal with credentials, so the requested headers are echoed.
    EXPECT_EQ(w->last_response.headers["Access-Control-Allow-Headers"], "X-Custom");
}

TEST(CorsTest, PlainOptionsRequestReachesRoutes) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::OPTIONS;
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_TRUE(m(req, w));
    EXPECT_FALSE(w->sent);
}

TEST(CorsTest, WildcardNeverGrantsCredentials) {
    middleware::CorsOptions o;
    o.allow_credentials = true; // allowed_origins defaults to {"*"}
    auto m = middleware::cors(o);
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers["Origin"] = "https://anyone.example";
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_TRUE(m(req, w));
    EXPECT_EQ(w->default_headers["Access-Control-Allow-Origin"], "*");
    EXPECT_EQ(w->default_headers.count("Access-Control-Allow-Credentials"), 0u);
    EXPECT_EQ(w->default_headers.count("Vary"), 0u);
}

TEST(CorsTest, NoOriginMeansNoCorsHeaders) {
    auto m = middleware::cors(allow_list());
    HttpRequest req;
    req.method = HttpMethod::GET;
    auto w = std::make_shared<CorsTestWriter>();
    EXPECT_TRUE(m(req, w));
    EXPECT_EQ(w->default_headers.count("Access-Control-Allow-Origin"), 0u);
}
