#include <gtest/gtest.h>
#include <orbit/http/HttpParser.hpp>

using namespace http;

TEST(HttpParserTest, ValidGetRequest) {
    std::string_view raw = 
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: curl/7.68.0\r\n"
        "Accept: */*\r\n"
        "\r\n";
        
    auto req = HttpParser::parse(raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, HttpMethod::GET);
    EXPECT_EQ(req->uri, "/index.html");
    EXPECT_EQ(req->http_version, "HTTP/1.1");
    EXPECT_EQ(req->headers["Host"], "localhost:8080");
    EXPECT_EQ(req->headers["User-Agent"], "curl/7.68.0");
    EXPECT_EQ(req->body, "");
}

TEST(HttpParserTest, ValidPostRequestWithBody) {
    std::string_view raw = 
        "POST /api/data HTTP/1.1\r\n"
        "Content-Length: 15\r\n"
        "\r\n"
        "{\"key\":\"value\"}";
        
    auto req = HttpParser::parse(raw);
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->method, HttpMethod::POST);
    EXPECT_EQ(req->uri, "/api/data");
    EXPECT_EQ(req->body, "{\"key\":\"value\"}");
}

TEST(HttpParserTest, MalformedRequestMissingCRLF) {
    std::string_view raw = "GET /index.html HTTP/1.1\nHost: localhost\n\n";
    auto req = HttpParser::parse(raw);
    EXPECT_FALSE(req.has_value());
}
