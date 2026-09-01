#include <gtest/gtest.h>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/middleware/JwtAuth.hpp>

using namespace http;
using namespace middleware;

// Mock ResponseWriter
class MockResponseWriter : public ResponseWriter {
public:
    HttpResponse last_response;
    bool sent = false;

    MockResponseWriter() : ResponseWriter(nullptr) {}

    void send(HttpResponse res) override {
        last_response = std::move(res);
        sent = true;
    }
    
    void send_headers(const HttpResponse& /*res*/) override {}
    void send_chunk(std::string_view /*chunk*/) override {}
    void end() override {}
};

TEST(MiddlewareTest, CorsHeadersInjected) {
    auto cors_mw = cors({"https://example.com"});
    
    HttpRequest req;
    req.method = HttpMethod::OPTIONS;
    req.headers["Origin"] = "https://example.com";
    auto writer = std::make_shared<MockResponseWriter>();
    
    bool proceed = cors_mw(req, writer);
    
    EXPECT_FALSE(proceed); // OPTIONS should be intercepted
    EXPECT_TRUE(writer->sent);
    EXPECT_EQ(writer->last_response.headers["Access-Control-Allow-Origin"], "https://example.com");
}

TEST(MiddlewareTest, JwtAuthBlocksUnauthorized) {
    auto jwt_mw = jwt_auth("secret_key");
    
    HttpRequest req; // Missing Auth header
    auto writer = std::make_shared<MockResponseWriter>();
    
    bool proceed = jwt_mw(req, writer);
    
    EXPECT_FALSE(proceed); // Intercepted
    EXPECT_TRUE(writer->sent);
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::Unauthorized);
}

TEST(MiddlewareTest, RateLimiterAllowsAndBlocks) {
    auto rl_mw = rate_limiter(2, std::chrono::seconds(10)); // 2 requests per 10s
    
    HttpRequest req;
    req.client_ip = "127.0.0.1";
    auto writer = std::make_shared<MockResponseWriter>();
    
    EXPECT_TRUE(rl_mw(req, writer)); // 1st OK
    EXPECT_TRUE(rl_mw(req, writer)); // 2nd OK
    
    bool third = rl_mw(req, writer); // 3rd Blocked
    EXPECT_FALSE(third);
    EXPECT_TRUE(writer->sent);
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::TooManyRequests);
}

TEST(MiddlewareTest, CsrfProtection) {
    auto csrf_mw = csrf_protection();
    
    HttpRequest req;
    req.method = HttpMethod::POST; // Modifying request requires CSRF token
    auto writer = std::make_shared<MockResponseWriter>();
    
    bool proceed = csrf_mw(req, writer);
    
    EXPECT_FALSE(proceed); // Blocked
    EXPECT_TRUE(writer->sent);
    EXPECT_EQ(writer->last_response.status_code, HttpStatus::Forbidden);
}
