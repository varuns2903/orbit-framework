#include <gtest/gtest.h>
#include <orbit/http/HttpParser.hpp>
#include <optional>
#include <string>

using namespace orbit::http;

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
    EXPECT_TRUE(orbit::http::connection_option_present("close", "close"));
    EXPECT_TRUE(orbit::http::connection_option_present("keep-alive", "keep-alive"));
}

TEST(ConnectionOptionTest, IsCaseInsensitive) {
    EXPECT_TRUE(orbit::http::connection_option_present("Close", "close"));
    EXPECT_TRUE(orbit::http::connection_option_present("CLOSE", "close"));
    EXPECT_TRUE(orbit::http::connection_option_present("Keep-Alive", "keep-alive"));
}

TEST(ConnectionOptionTest, FindsTokenInCommaSeparatedList) {
    EXPECT_TRUE(orbit::http::connection_option_present("keep-alive, TE", "keep-alive"));
    EXPECT_TRUE(orbit::http::connection_option_present("TE, close", "close"));
    EXPECT_TRUE(orbit::http::connection_option_present("upgrade, close, TE", "close"));
}

TEST(ConnectionOptionTest, TolerateSurroundingWhitespace) {
    EXPECT_TRUE(orbit::http::connection_option_present("  close  ", "close"));
    EXPECT_TRUE(orbit::http::connection_option_present("TE,\tclose", "close"));
}

TEST(ConnectionOptionTest, DoesNotMatchSubstrings) {
    // "close" must not be found inside a longer token.
    EXPECT_FALSE(orbit::http::connection_option_present("closer", "close"));
    EXPECT_FALSE(orbit::http::connection_option_present("not-close", "close"));
    EXPECT_FALSE(orbit::http::connection_option_present("keep-alive", "close"));
}

TEST(ConnectionOptionTest, HandlesEmptyAndDegenerateInput) {
    EXPECT_FALSE(orbit::http::connection_option_present("", "close"));
    EXPECT_FALSE(orbit::http::connection_option_present(",", "close"));
    EXPECT_FALSE(orbit::http::connection_option_present(" , , ", "close"));
}

// --- HTTP version parsing, which drives connection persistence ---

TEST(HttpParserTest, RecordsHttpVersionOneZero) {
    auto req = orbit::http::HttpParser::parse("GET / HTTP/1.0\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->http_version, "HTTP/1.0");
}

TEST(HttpParserTest, RecordsHttpVersionOneOne) {
    auto req = orbit::http::HttpParser::parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
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

TEST(UrlEncodedTest, DecodesPairs) {
    std::unordered_map<std::string, std::string> out;
    ASSERT_TRUE(orbit::http::parse_urlencoded("a=1&b=two+words&c=%26%3D&empty=&flag&&x=%E2%9C%93", out));
    EXPECT_EQ(out["a"], "1");
    EXPECT_EQ(out["b"], "two words");
    EXPECT_EQ(out["c"], "&=");
    EXPECT_EQ(out["empty"], "");
    EXPECT_TRUE(out.count("flag"));
    EXPECT_EQ(out["flag"], "");
    EXPECT_EQ(out["x"], "\xE2\x9C\x93");
    EXPECT_EQ(out.size(), 6u);
}

TEST(UrlEncodedTest, LastValueWinsAndValueKeepsLaterEquals) {
    std::unordered_map<std::string, std::string> out;
    ASSERT_TRUE(orbit::http::parse_urlencoded("k=1&k=2&e=a=b", out));
    EXPECT_EQ(out["k"], "2");
    EXPECT_EQ(out["e"], "a=b");
}

TEST(UrlEncodedTest, MalformedEscapeFails) {
    std::unordered_map<std::string, std::string> out;
    EXPECT_FALSE(orbit::http::parse_urlencoded("a=%zz", out));
    EXPECT_FALSE(orbit::http::parse_urlencoded("a=%4", out));
    EXPECT_TRUE(orbit::http::parse_urlencoded("", out));
}

TEST(FormFieldsTest, OnlyForUrlEncodedBodies) {
    orbit::http::HttpRequest req;
    req.body = "name=Ada+Lovelace&lang=c%2B%2B";
    EXPECT_TRUE(req.form_fields().empty()); // no Content-Type

    req.set_header("Content-Type", "application/json");
    EXPECT_TRUE(req.form_fields().empty());

    req.set_header("Content-Type", "Application/X-WWW-Form-URLEncoded ; charset=UTF-8");
    auto fields = req.form_fields();
    EXPECT_EQ(fields["name"], "Ada Lovelace");
    EXPECT_EQ(fields["lang"], "c++");

    req.set_header("Content-Type", "application/x-www-form-urlencoded-extra");
    EXPECT_TRUE(req.form_fields().empty());
}

TEST(FormFieldsTest, MalformedBodyGivesNoFields) {
    orbit::http::HttpRequest req;
    req.set_header("Content-Type", "application/x-www-form-urlencoded");
    req.body = "ok=1&bad=%G0";
    EXPECT_TRUE(req.form_fields().empty());
}

// HttpParser::parse runs on Http1Parser: the same strict framing as a
// connection, and a request that owns its data.

TEST(HttpParserFramingTest, AmbiguousFramingIsRejected) {
    EXPECT_FALSE(HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: abc\r\n\r\nA").has_value());
    EXPECT_FALSE(HttpParser::parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n").has_value());
}

TEST(HttpParserFramingTest, BodyIsSlicedToContentLength) {
    auto req = HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /next HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->body, "abc");
}

TEST(HttpParserFramingTest, NoFramingHeadersMeansNoBody) {
    auto req = HttpParser::parse("GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /next HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_TRUE(req->body.empty());
}

TEST(HttpParserFramingTest, ChunkedBodyIsDecoded) {
    auto req = HttpParser::parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->body, "abcde");
}

TEST(HttpParserFramingTest, IncompleteRequestIsNotParsed) {
    EXPECT_FALSE(HttpParser::parse("GET / HTTP/1.1\r\nHost: a\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab").has_value());
}

TEST(HttpParserFramingTest, RequestOutlivesTheInput) {
    std::optional<orbit::http::HttpRequest> req;
    {
        std::string raw = "POST /p?x=1 HTTP/1.1\r\nHost: owned\r\nCookie: a=b\r\nContent-Length: 4\r\n\r\nbody";
        req = HttpParser::parse(raw);
        raw.assign(raw.size(), '#');
    }
    ASSERT_TRUE(req.has_value());
    auto moved = std::move(*req); // moving keeps the owned storage in place
    EXPECT_EQ(moved.headers.at("Host"), "owned");
    EXPECT_EQ(moved.body, "body");
    EXPECT_EQ(moved.query.at("x"), "1");
    EXPECT_EQ(moved.cookies.at("a"), "b");
}
