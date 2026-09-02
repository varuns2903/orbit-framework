#include <gtest/gtest.h>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <memory>
#include <chrono>

using namespace middleware;
using namespace http;

class MockResponseWriter : public ResponseWriter {
public:
    HttpResponse last_response;
    bool ended = false;
    
    void send(HttpResponse&& response) override {
        last_response = std::move(response);
    }
    void send_headers(HttpResponse& response) override {
        last_response = response;
    }
    void write_chunk(std::string_view chunk) override {}
    void end() override { ended = true; }
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override {}
    void set_header(const std::string& key, const std::string& value) override {
        last_response.headers[key] = value;
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
    auto writer = std::make_shared<MockResponseWriter>();
    
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
    auto writer = std::make_shared<MockResponseWriter>();
    
    // First request
    EXPECT_TRUE(m(req, writer));
    
    // Second request
    EXPECT_TRUE(m(req, writer));
    
    // Third request (should be blocked)
    EXPECT_FALSE(m(req, writer));
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::TooManyRequests);
}
