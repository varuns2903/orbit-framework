#include <gtest/gtest.h>
#include <orbit/http/HttpResponse.hpp>

using namespace orbit::http;

namespace {

std::string status_line(HttpResponse& res) {
    std::string s = res.serialize_headers();
    return s.substr(0, s.find("\r\n"));
}

} // namespace

TEST(ResponseSerializationTest, EveryEnumeratedStatusHasAStatusLine) {
    for (int code : {100, 101, 200, 201, 202, 204, 206, 301, 302, 303, 304, 307, 308, 400, 401, 403, 404, 405,
                     406, 408, 409, 410, 411, 412, 413, 414, 415, 416, 417, 422, 426, 429, 431, 500, 501, 502, 503, 504}) {
        HttpResponse res;
        res.status_code = static_cast<HttpStatus>(code);
        std::string line = status_line(res);
        EXPECT_EQ(line.rfind("HTTP/1.1 " + std::to_string(code) + " ", 0), 0u) << line;
        EXPECT_GT(line.size(), 13u) << line; // has a reason phrase
    }
}

TEST(ResponseSerializationTest, UnregisteredCodesGetClassReason) {
    HttpResponse res;
    res.status_code = static_cast<HttpStatus>(499);
    EXPECT_EQ(status_line(res), "HTTP/1.1 499 Client Error");
    res.status_code = static_cast<HttpStatus>(599);
    EXPECT_EQ(status_line(res), "HTTP/1.1 599 Server Error");
}

TEST(ResponseSerializationTest, OutOfRangeCodeBecomes500) {
    HttpResponse res;
    res.status_code = static_cast<HttpStatus>(42);
    EXPECT_EQ(status_line(res), "HTTP/1.1 500 Internal Server Error");
}

TEST(ResponseSerializationTest, HeaderValueWithCrlfIsDropped) {
    HttpResponse res;
    res.status(HttpStatus::Found);
    res.headers["Location"] = "/next\r\nSet-Cookie: admin=1";
    res.headers["X-Safe"] = "ok";
    std::string out = res.serialize_headers();
    EXPECT_EQ(out.find("admin=1"), std::string::npos) << out;
    EXPECT_EQ(out.find("Location"), std::string::npos) << out;
    EXPECT_NE(out.find("X-Safe: ok\r\n"), std::string::npos) << out;
}

TEST(ResponseSerializationTest, InvalidHeaderNameIsDropped) {
    HttpResponse res;
    res.headers["Bad Name"] = "x";
    res.headers["X-Evil\r\nInjected"] = "y";
    std::string out = res.serialize_headers();
    EXPECT_EQ(out.find("Bad Name"), std::string::npos);
    EXPECT_EQ(out.find("Injected"), std::string::npos);
}

TEST(ResponseSerializationTest, CookieWithInjectedAttributesIsDropped) {
    HttpResponse res;
    Cookie evil;
    evil.name = "theme";
    evil.value = "dark; Domain=evil.example";
    res.set_cookie(evil);
    Cookie crlf;
    crlf.name = "a";
    crlf.value = "b\r\nX-Injected: 1";
    res.set_cookie(crlf);
    Cookie good;
    good.name = "lang";
    good.value = "en";
    res.set_cookie(good);
    std::string out = res.serialize_headers();
    EXPECT_EQ(out.find("evil.example"), std::string::npos) << out;
    EXPECT_EQ(out.find("X-Injected"), std::string::npos) << out;
    EXPECT_NE(out.find("Set-Cookie: lang=en; Path=/\r\n"), std::string::npos) << out;
}

TEST(ResponseSerializationTest, LowercaseContentLengthIsNotDuplicated) {
    HttpResponse res;
    res.body = "hello";
    res.headers["content-length"] = "5";
    std::string out = res.serialize_headers();
    size_t first = out.find("ontent-")
        , second = out.find("ontent-", first + 1);
    EXPECT_NE(first, std::string::npos);
    EXPECT_EQ(second, std::string::npos) << out;
}

TEST(ResponseSerializationTest, HeaderValidators) {
    EXPECT_TRUE(is_valid_header_name("X-Request-ID"));
    EXPECT_FALSE(is_valid_header_name(""));
    EXPECT_FALSE(is_valid_header_name("X:Y"));
    EXPECT_TRUE(is_valid_header_value("text/html; charset=utf-8"));
    EXPECT_FALSE(is_valid_header_value("a\nb"));
    EXPECT_FALSE(is_valid_header_value(std::string("a\0b", 3)));
}

// Without a length, an HTTP/1.1 client reads an empty-bodied response until
// the connection closes (RFC 9112 section 6.3): a keep-alive client sat
// waiting for the idle timeout after every redirect.
TEST(ResponseSerializationTest, EmptyBodyStatesZeroLength) {
    orbit::http::HttpResponse redirect;
    redirect.status(orbit::http::HttpStatus::Found);
    redirect.headers["Location"] = "/elsewhere";
    EXPECT_NE(redirect.serialize().find("\r\nContent-Length: 0\r\n"), std::string::npos) << redirect.serialize();

    orbit::http::HttpResponse created;
    created.status(orbit::http::HttpStatus::Created);
    EXPECT_NE(created.serialize().find("Content-Length: 0"), std::string::npos);
}

TEST(ResponseSerializationTest, NoLengthForBodilessOrChunkedResponses) {
    orbit::http::HttpResponse no_content;
    no_content.status(orbit::http::HttpStatus::NoContent);
    EXPECT_EQ(no_content.serialize().find("Content-Length"), std::string::npos);

    orbit::http::HttpResponse not_modified;
    not_modified.status(orbit::http::HttpStatus::NotModified);
    EXPECT_EQ(not_modified.serialize().find("Content-Length"), std::string::npos);

    orbit::http::HttpResponse streamed;
    streamed.headers["Transfer-Encoding"] = "chunked";
    EXPECT_EQ(streamed.serialize_headers().find("Content-Length"), std::string::npos);
}

TEST(ResponseSerializationTest, ExplicitLengthIsNotDuplicated) {
    orbit::http::HttpResponse res;
    res.headers["Content-Length"] = "5"; // e.g. HEAD for a 5-byte resource
    std::string out = res.serialize_headers();
    EXPECT_EQ(out.find("Content-Length"), out.rfind("Content-Length"));
}

// RFC 9110 section 6.6.1 (#170): an origin server with a clock sends Date.
TEST(ResponseSerializationTest, EveryResponseCarriesDate) {
    for (auto status : {orbit::http::HttpStatus::OK, orbit::http::HttpStatus::NoContent,
                        orbit::http::HttpStatus::NotFound, orbit::http::HttpStatus::InternalServerError}) {
        orbit::http::HttpResponse res;
        res.status(status);
        const std::string out = res.serialize_headers();
        const size_t at = out.find("\r\nDate: ");
        ASSERT_NE(at, std::string::npos) << out;
        const size_t end = out.find("\r\n", at + 2);
        EXPECT_EQ(out.substr(at + 8, end - at - 8), orbit::http::http_date_now());
    }
}

TEST(ResponseSerializationTest, HandlerDateIsKeptNotDoubled) {
    orbit::http::HttpResponse res;
    res.headers["date"] = "Sun, 06 Nov 1994 08:49:37 GMT";
    const std::string out = res.serialize_headers();
    EXPECT_NE(out.find("date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("Date: "), std::string::npos) << out;
}

TEST(ResponseSerializationTest, HttpDateIsImfFixdate) {
    EXPECT_EQ(orbit::http::format_http_date(784111777), "Sun, 06 Nov 1994 08:49:37 GMT");
    EXPECT_EQ(orbit::http::format_http_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");

    // The cached value is the current second, formatted the same way.
    const std::time_t before = std::time(nullptr);
    const std::string now(orbit::http::http_date_now());
    const std::time_t after = std::time(nullptr);
    EXPECT_TRUE(now == orbit::http::format_http_date(before) || now == orbit::http::format_http_date(after)) << now;
}

// --- json(j, status) and error(status, msg) (#211) ---

TEST(ResponseSerializationTest, JsonWithStatusSetsBothAtOnce) {
    nlohmann::json body_true = {{"ok", true}};
    orbit::http::HttpResponse lvalue;
    lvalue.json(body_true, orbit::http::HttpStatus::Created);
    EXPECT_EQ(lvalue.status_code, orbit::http::HttpStatus::Created);
    EXPECT_EQ(lvalue.body, body_true.dump());
    EXPECT_EQ(lvalue.headers.at("Content-Type"), "application/json");

    nlohmann::json body_false = {{"ok", false}};
    orbit::http::HttpResponse rvalue = orbit::http::HttpResponse().json(body_false, orbit::http::HttpStatus::Accepted);
    EXPECT_EQ(rvalue.status_code, orbit::http::HttpStatus::Accepted);
    EXPECT_EQ(rvalue.body, body_false.dump());
}

TEST(ResponseSerializationTest, ErrorBuildsAJsonErrorBody) {
    orbit::http::HttpResponse res = orbit::http::HttpResponse::error(orbit::http::HttpStatus::NotFound,
                                                                     "task not found");
    EXPECT_EQ(res.status_code, orbit::http::HttpStatus::NotFound);
    EXPECT_EQ(res.headers.at("Content-Type"), "application/json");
    auto parsed = nlohmann::json::parse(res.body);
    EXPECT_EQ(parsed["error"], "task not found");
}

// A quote or backslash in the message stays valid JSON (built with
// nlohmann, not concatenated; same bug class as #188).
TEST(ResponseSerializationTest, ErrorMessageWithSpecialCharactersStaysValidJson) {
    orbit::http::HttpResponse res = orbit::http::HttpResponse::error(
        orbit::http::HttpStatus::BadRequest, R"(bad "field" with a \ in it)");
    auto parsed = nlohmann::json::parse(res.body, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded()) << res.body;
    EXPECT_EQ(parsed["error"], R"(bad "field" with a \ in it)");
}
