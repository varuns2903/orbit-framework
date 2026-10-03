#include <gtest/gtest.h>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <memory>
#include <chrono>

using namespace middleware;
using namespace http;

class MiddlewareMockResponseWriter : public ResponseWriter {
public:
    HttpResponse last_response;
    bool ended = false;
    std::unordered_map<std::string, std::string> default_headers_;
    
    void send(HttpResponse&& response) override {
        last_response = std::move(response);
        for (const auto& [k, v] : default_headers_) {
            if (last_response.headers.find(k) == last_response.headers.end()) {
                last_response.headers[k] = v;
            }
        }
    }
    void send_headers(HttpResponse& response) override {
        last_response.status_code = response.status_code;
        last_response.headers = response.headers;
    }
    void write_chunk(std::string_view chunk) override {}
    void end() override { ended = true; }
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override {}
    void set_header(const std::string& key, const std::string& value) override {
        default_headers_[key] = value;
    }
    network::Proactor& proactor() override { throw std::runtime_error("Not implemented"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("Not implemented"); }
    void send_sse_event(std::string_view data, std::string_view event, std::string_view id) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) override {}
    void read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) override {}
};

TEST(MiddlewareTest, CorsMiddleware) {
    auto m = cors();
    HttpRequest req;
    req.method = HttpMethod::OPTIONS;
    req.headers["Origin"] = "http://example.com";
    req.headers["Access-Control-Request-Method"] = "POST";
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();
    
    bool continue_chain = m(req, writer);
    EXPECT_FALSE(continue_chain);
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::NoContent);
    EXPECT_EQ(writer->last_response.headers["Access-Control-Allow-Origin"], "*");
    
    req.method = HttpMethod::GET;
    continue_chain = m(req, writer);
    EXPECT_TRUE(continue_chain);
}

TEST(MiddlewareTest, RateLimiterAllowsRequests) {
    auto m = rate_limit(2, std::chrono::seconds(60));
    HttpRequest req;
    req.client_ip = "127.0.0.1";
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();
    
    // First request
    EXPECT_TRUE(m(req, writer));
    
    // Second request
    EXPECT_TRUE(m(req, writer));
    
    // Third request (should be blocked)
    EXPECT_FALSE(m(req, writer));
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::TooManyRequests);
}

// Csrf used to store views of local strings in req.headers; reading the
// header after the middleware returned read freed memory (caught by ASan).
TEST(MiddlewareTest, CsrfInjectedHeaderOutlivesMiddleware) {
    auto m = csrf_protection();
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    HttpRequest fresh;
    fresh.method = HttpMethod::GET;
    ASSERT_TRUE(m(fresh, writer));
    std::string injected(fresh.headers["X-CSRF-Token"]);
    EXPECT_FALSE(injected.empty());

    HttpRequest returning;
    returning.method = HttpMethod::GET;
    returning.cookies["csrf_token"] = "existing-token-value-from-cookie";
    ASSERT_TRUE(m(returning, writer));
    EXPECT_EQ(returning.headers["X-CSRF-Token"], "existing-token-value-from-cookie");
}

TEST(HttpRequestTest, SetHeaderOwnsNameAndValue) {
    HttpRequest req;
    {
        std::string name = "X-Temporary-Header-Name";
        std::string value = "a value long enough to live on the heap, not in SSO";
        req.set_header(name, value);
        for (int i = 0; i < 100; ++i) req.set_header("X-" + std::to_string(i), std::to_string(i));
    }
    EXPECT_EQ(req.headers["x-temporary-header-name"], "a value long enough to live on the heap, not in SSO");
    EXPECT_EQ(req.headers["X-0"], "0");
    req.set_header("x-0", "replaced");
    EXPECT_EQ(req.headers["X-0"], "replaced");
}
