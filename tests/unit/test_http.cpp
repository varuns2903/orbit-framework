#include <gtest/gtest.h>
#include <orbit/http/HttpParser.hpp>
#include <orbit/http/HttpResponse.hpp>

using namespace http;

// ==================== HttpParser Edge Cases ====================

TEST(HttpParserEdgeCasesTest, EmptyRequest) {
    std::string_view raw = "";
    auto result = HttpParser::parse(raw);
    EXPECT_FALSE(result.has_value());
}

TEST(HttpParserEdgeCasesTest, PartialRequestLine) {
    std::string_view raw = "GET /path";
    auto result = HttpParser::parse(raw);
    EXPECT_FALSE(result.has_value());
}

TEST(HttpParserEdgeCasesTest, GetWithHeaders) {
    std::string raw = "GET /test HTTP/1.1\r\nHost: localhost\r\nAccept: text/html\r\nX-Custom: value123\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::GET);
    EXPECT_EQ(result->uri, "/test");
    EXPECT_EQ(result->headers["Host"], "localhost");
    EXPECT_EQ(result->headers["Accept"], "text/html");
    EXPECT_EQ(result->headers["X-Custom"], "value123");
}

TEST(HttpParserEdgeCasesTest, PostWithJsonBody) {
    std::string raw = "POST /api/data HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n{\"key\":\"val\"}";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::POST);
    EXPECT_EQ(result->body, "{\"key\":\"val\"}");
}

TEST(HttpParserEdgeCasesTest, PutMethod) {
    std::string raw = "PUT /resource/1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::PUT);
    EXPECT_EQ(result->uri, "/resource/1");
}

TEST(HttpParserEdgeCasesTest, DeleteMethod) {
    std::string raw = "DELETE /resource/1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::DELETE);
}

TEST(HttpParserEdgeCasesTest, PatchMethod) {
    std::string raw = "PATCH /resource/1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::PATCH);
}

TEST(HttpParserEdgeCasesTest, OptionsMethod) {
    std::string raw = "OPTIONS /api HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::OPTIONS);
}

TEST(HttpParserEdgeCasesTest, HeadMethod) {
    std::string raw = "HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->method, HttpMethod::HEAD);
}

TEST(HttpParserEdgeCasesTest, QueryString) {
    std::string raw = "GET /search?q=hello&page=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    // Parser strips query string from URI
    EXPECT_EQ(result->uri, "/search");
}

TEST(HttpParserEdgeCasesTest, LargeBody) {
    std::string body(4096, 'A');
    std::string raw = "POST /upload HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    auto result = HttpParser::parse(raw);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->body.size(), 4096u);
}

// ==================== HttpResponse Tests ====================

TEST(HttpResponseTest, DefaultStatus) {
    HttpResponse res;
    res.status(HttpStatus::OK);
    EXPECT_EQ(res.status_code, HttpStatus::OK);
}

TEST(HttpResponseTest, SetBody) {
    HttpResponse res;
    res.send("Hello");
    EXPECT_EQ(res.body, "Hello");
}

TEST(HttpResponseTest, ChainedApi) {
    HttpResponse res;
    res.status(HttpStatus::Created).send("Created!");
    EXPECT_EQ(res.status_code, HttpStatus::Created);
    EXPECT_EQ(res.body, "Created!");
}

TEST(HttpResponseTest, JsonResponse) {
    HttpResponse res;
    nlohmann::json j = {{"key", "value"}};
    res.status(HttpStatus::OK).set_body(j.dump(), "application/json");
    EXPECT_EQ(res.headers["Content-Type"], "application/json");
    EXPECT_NE(res.body.find("key"), std::string::npos);
}

TEST(HttpResponseTest, Serialization) {
    HttpResponse res;
    res.status(HttpStatus::OK).send("test body");
    res.headers["X-Custom"] = "test";
    std::string serialized = res.serialize();
    EXPECT_NE(serialized.find("200"), std::string::npos);
    EXPECT_NE(serialized.find("X-Custom: test"), std::string::npos);
    EXPECT_NE(serialized.find("test body"), std::string::npos);
}

TEST(HttpResponseTest, NotFoundStatus) {
    HttpResponse res;
    res.status(HttpStatus::NotFound).send("Not Found");
    EXPECT_EQ(res.status_code, HttpStatus::NotFound);
}

TEST(HttpResponseTest, InternalServerError) {
    HttpResponse res;
    res.status(HttpStatus::InternalServerError).send("Server Error");
    EXPECT_EQ(res.status_code, HttpStatus::InternalServerError);
}
