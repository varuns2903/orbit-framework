#include <gtest/gtest.h>
#include <orbit/http/HttpResponse.hpp>

using namespace http;

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
    http::HttpResponse redirect;
    redirect.status(http::HttpStatus::Found);
    redirect.headers["Location"] = "/elsewhere";
    EXPECT_NE(redirect.serialize().find("\r\nContent-Length: 0\r\n"), std::string::npos) << redirect.serialize();

    http::HttpResponse created;
    created.status(http::HttpStatus::Created);
    EXPECT_NE(created.serialize().find("Content-Length: 0"), std::string::npos);
}

TEST(ResponseSerializationTest, NoLengthForBodilessOrChunkedResponses) {
    http::HttpResponse no_content;
    no_content.status(http::HttpStatus::NoContent);
    EXPECT_EQ(no_content.serialize().find("Content-Length"), std::string::npos);

    http::HttpResponse not_modified;
    not_modified.status(http::HttpStatus::NotModified);
    EXPECT_EQ(not_modified.serialize().find("Content-Length"), std::string::npos);

    http::HttpResponse streamed;
    streamed.headers["Transfer-Encoding"] = "chunked";
    EXPECT_EQ(streamed.serialize_headers().find("Content-Length"), std::string::npos);
}

TEST(ResponseSerializationTest, ExplicitLengthIsNotDuplicated) {
    http::HttpResponse res;
    res.headers["Content-Length"] = "5"; // e.g. HEAD for a 5-byte resource
    std::string out = res.serialize_headers();
    EXPECT_EQ(out.find("Content-Length"), out.rfind("Content-Length"));
}
