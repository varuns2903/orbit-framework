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
#include <string>
#include <thread>

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

TEST(MiddlewareTest, RateLimiterSendsRetryAfter) {
    // 1 request per 60s: the next token arrives in about 60 seconds.
    auto m = rate_limit(1, std::chrono::seconds(60));
    HttpRequest req;
    req.client_ip = "10.0.0.1";
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    EXPECT_TRUE(m(req, writer));
    EXPECT_FALSE(m(req, writer));
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::TooManyRequests);
    ASSERT_EQ(writer->last_response.headers.count("Retry-After"), 1u);
    int retry_after = std::stoi(writer->last_response.headers["Retry-After"]);
    EXPECT_GE(retry_after, 59);
    EXPECT_LE(retry_after, 60);
    // Rate limiting must not tear down keep-alive connections.
    EXPECT_EQ(writer->last_response.headers.count("Connection"), 0u);
}

TEST(MiddlewareTest, RateLimiterTracksClientsSeparately) {
    auto m = rate_limit(1, std::chrono::seconds(60));
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();
    HttpRequest a;
    a.client_ip = "10.0.0.1";
    HttpRequest b;
    b.client_ip = "10.0.0.2";

    EXPECT_TRUE(m(a, writer));
    EXPECT_FALSE(m(a, writer));
    EXPECT_TRUE(m(b, writer));
}

TEST(MiddlewareTest, RateLimiterUsesCustomKey) {
    RateLimitOptions opts;
    opts.max_requests = 1;
    opts.window = std::chrono::seconds(60);
    opts.key = [](const HttpRequest& req) {
        auto it = req.headers.find("X-Api-Key");
        return it != req.headers.end() ? std::string(it->second) : std::string();
    };
    auto m = rate_limit(opts);
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    // Same IP, different API keys: limited per key.
    HttpRequest a;
    a.client_ip = "10.0.0.1";
    a.headers["X-Api-Key"] = "alpha";
    HttpRequest b;
    b.client_ip = "10.0.0.1";
    b.headers["X-Api-Key"] = "beta";

    EXPECT_TRUE(m(a, writer));
    EXPECT_FALSE(m(a, writer));
    EXPECT_TRUE(m(b, writer));
}

TEST(MiddlewareTest, RateLimiterRefillsFractionally) {
    // 2 per second: one token every 500 ms, rather than all at once after the window.
    RateLimiter limiter(2, std::chrono::seconds(1));
    HttpRequest req;
    req.client_ip = "10.0.0.1";
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    EXPECT_TRUE(limiter(req, writer));
    EXPECT_TRUE(limiter(req, writer));
    EXPECT_FALSE(limiter(req, writer));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_TRUE(limiter(req, writer));
    EXPECT_FALSE(limiter(req, writer));
}

TEST(MiddlewareTest, RateLimiterMemoryIsBounded) {
    RateLimitOptions opts;
    opts.max_requests = 5;
    opts.window = std::chrono::seconds(60);
    opts.max_tracked_clients = 100;
    RateLimiter limiter(opts);
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    // A spoofed-IP flood must not grow the table without limit.
    for (int i = 0; i < 10000; ++i) {
        HttpRequest req;
        req.client_ip = "10." + std::to_string(i / 65536) + "." + std::to_string((i / 256) % 256) + "." + std::to_string(i % 256);
        EXPECT_TRUE(limiter(req, writer));
    }
    EXPECT_LE(limiter.tracked_clients(), 100u);
}

TEST(MiddlewareTest, RateLimiterForgetsRefilledClients) {
    RateLimitOptions opts;
    opts.max_requests = 100;
    opts.window = std::chrono::seconds(1);
    RateLimiter limiter(opts);
    auto writer = std::make_shared<MiddlewareMockResponseWriter>();

    for (int i = 0; i < 50; ++i) {
        HttpRequest req;
        req.client_ip = "10.0.0." + std::to_string(i);
        EXPECT_TRUE(limiter(req, writer));
    }
    EXPECT_EQ(limiter.tracked_clients(), 50u);

    // After a full window every bucket has refilled, so the next sweep drops them.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    HttpRequest req;
    req.client_ip = "10.0.1.1";
    EXPECT_TRUE(limiter(req, writer));
    EXPECT_EQ(limiter.tracked_clients(), 1u);
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
