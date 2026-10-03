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

// --- Connection header option parsing (RFC 9110 section 7.6.1) ---

TEST(ConnectionOptionTest, MatchesSingleToken) {
    EXPECT_TRUE(http::connection_option_present("close", "close"));
    EXPECT_TRUE(http::connection_option_present("keep-alive", "keep-alive"));
}

TEST(ConnectionOptionTest, IsCaseInsensitive) {
    EXPECT_TRUE(http::connection_option_present("Close", "close"));
    EXPECT_TRUE(http::connection_option_present("CLOSE", "close"));
    EXPECT_TRUE(http::connection_option_present("Keep-Alive", "keep-alive"));
}

TEST(ConnectionOptionTest, FindsTokenInCommaSeparatedList) {
    EXPECT_TRUE(http::connection_option_present("keep-alive, TE", "keep-alive"));
    EXPECT_TRUE(http::connection_option_present("TE, close", "close"));
    EXPECT_TRUE(http::connection_option_present("upgrade, close, TE", "close"));
}

TEST(ConnectionOptionTest, TolerateSurroundingWhitespace) {
    EXPECT_TRUE(http::connection_option_present("  close  ", "close"));
    EXPECT_TRUE(http::connection_option_present("TE,\tclose", "close"));
}

TEST(ConnectionOptionTest, DoesNotMatchSubstrings) {
    // "close" must not be found inside a longer token.
    EXPECT_FALSE(http::connection_option_present("closer", "close"));
    EXPECT_FALSE(http::connection_option_present("not-close", "close"));
    EXPECT_FALSE(http::connection_option_present("keep-alive", "close"));
}

TEST(ConnectionOptionTest, HandlesEmptyAndDegenerateInput) {
    EXPECT_FALSE(http::connection_option_present("", "close"));
    EXPECT_FALSE(http::connection_option_present(",", "close"));
    EXPECT_FALSE(http::connection_option_present(" , , ", "close"));
}

// --- HTTP version parsing, which drives connection persistence ---

TEST(HttpParserTest, RecordsHttpVersionOneZero) {
    auto req = http::HttpParser::parse("GET / HTTP/1.0\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->http_version, "HTTP/1.0");
}

TEST(HttpParserTest, RecordsHttpVersionOneOne) {
    auto req = http::HttpParser::parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->http_version, "HTTP/1.1");
}

// --- Percent-decoding (issue #24) ---

TEST(PercentDecodingTest, PathIsDecoded) {
    auto req = HttpParser::parse("GET /files/my%20doc%C3%A9.txt HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->uri, "/files/my doc\xC3\xA9.txt");
}

TEST(PercentDecodingTest, QueryKeysAndValuesAreDecoded) {
    auto req = HttpParser::parse("GET /s?q=a%26b&name=John+Doe&empty&%6Bey=v%3D1 HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->uri, "/s");
    EXPECT_EQ(req->query["q"], "a&b");
    EXPECT_EQ(req->query["name"], "John Doe");
    EXPECT_EQ(req->query["empty"], "");
    EXPECT_EQ(req->query["key"], "v=1");
}

TEST(PercentDecodingTest, PlusIsLiteralInPath) {
    auto req = HttpParser::parse("GET /c++/a+b HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->uri, "/c++/a+b");
}

TEST(PercentDecodingTest, RejectsEncodedSeparatorsAndNulInPath) {
    EXPECT_FALSE(HttpParser::parse("GET /a%2F..%2Fetc HTTP/1.1\r\n\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("GET /a%2f..%2fetc HTTP/1.1\r\n\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("GET /a%5C..%5Cetc HTTP/1.1\r\n\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("GET /a%00.txt HTTP/1.1\r\n\r\n").has_value());
}

TEST(PercentDecodingTest, RejectsMalformedEscapes) {
    EXPECT_FALSE(HttpParser::parse("GET /a%zz HTTP/1.1\r\n\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("GET /a% HTTP/1.1\r\n\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("GET /a?q=%4 HTTP/1.1\r\n\r\n").has_value());
}

TEST(PercentDecodingTest, SlashAllowedInQuery) {
    auto req = HttpParser::parse("GET /cb?code=4%2F0Ab HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->query["code"], "4/0Ab");
}
