#include <gtest/gtest.h>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace middleware;
using namespace http;

namespace {

class CsrfMockResponseWriter : public ResponseWriter {
public:
    std::optional<HttpResponse> sent;
    std::vector<std::function<void(HttpResponse&)>> interceptors;

    // The response a handler would send after the middleware, with every
    // interceptor applied as the real writer does.
    HttpResponse intercepted() {
        HttpResponse res;
        for (auto& i : interceptors) i(res);
        return res;
    }

    void send(HttpResponse&& response) override { sent = std::move(response); }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override {
        interceptors.push_back(std::move(interceptor));
    }
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("Not implemented"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("Not implemented"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

const std::string kToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

HttpRequest mutating(HttpMethod method = HttpMethod::POST) {
    HttpRequest req;
    req.method = method;
    return req;
}

void expect_rejected(const CsrfMockResponseWriter& writer) {
    ASSERT_TRUE(writer.sent.has_value());
    EXPECT_EQ(writer.sent->status_code, HttpStatus::Forbidden);
    EXPECT_EQ(writer.sent->body, "CSRF Token Verification Failed");
}

bool is_lower_hex(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

} // namespace

// --- Safe methods: issue or echo the token ---

TEST(CsrfTest, FirstSafeRequestIssuesATokenCookie) {
    Csrf csrf;
    HttpRequest req;
    req.method = HttpMethod::GET;
    auto writer = std::make_shared<CsrfMockResponseWriter>();

    ASSERT_TRUE(csrf(req, writer));
    EXPECT_FALSE(writer->sent.has_value());

    HttpResponse res = writer->intercepted();
    ASSERT_EQ(res.cookies.size(), 1u);
    const Cookie& c = res.cookies[0];
    EXPECT_EQ(c.name, "csrf_token");
    EXPECT_EQ(c.path, "/");
    EXPECT_EQ(c.same_site, "Lax");
    EXPECT_EQ(c.value.size(), 64u);
    EXPECT_TRUE(is_lower_hex(c.value)) << c.value;

    // Handlers and templates read the same token from the request.
    EXPECT_EQ(req.headers["X-CSRF-Token"], c.value);
}

TEST(CsrfTest, IssuedTokensDiffer) {
    Csrf csrf;
    auto token_for_new_visitor = [&] {
        HttpRequest req;
        req.method = HttpMethod::GET;
        auto writer = std::make_shared<CsrfMockResponseWriter>();
        csrf(req, writer);
        return writer->intercepted().cookies.at(0).value;
    };
    EXPECT_NE(token_for_new_visitor(), token_for_new_visitor());
}

TEST(CsrfTest, SafeRequestWithCookieEchoesItWithoutANewCookie) {
    Csrf csrf;
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.cookies["csrf_token"] = kToken;
    auto writer = std::make_shared<CsrfMockResponseWriter>();

    ASSERT_TRUE(csrf(req, writer));
    EXPECT_TRUE(writer->intercepted().cookies.empty());
    EXPECT_EQ(req.headers["X-CSRF-Token"], kToken);
}

TEST(CsrfTest, HeadAndOptionsAreSafe) {
    Csrf csrf;
    for (HttpMethod m : {HttpMethod::HEAD, HttpMethod::OPTIONS}) {
        HttpRequest req;
        req.method = m;
        auto writer = std::make_shared<CsrfMockResponseWriter>();
        EXPECT_TRUE(csrf(req, writer));
        EXPECT_FALSE(writer->sent.has_value());
        EXPECT_EQ(writer->intercepted().cookies.size(), 1u);
    }
}

// --- Mutating methods: verify the token ---

TEST(CsrfTest, EveryOtherMethodIsVerified) {
    Csrf csrf;
    for (HttpMethod m : {HttpMethod::POST, HttpMethod::PUT, HttpMethod::PATCH,
                         HttpMethod::DELETE, HttpMethod::UNKNOWN}) {
        HttpRequest req = mutating(m);
        req.cookies["csrf_token"] = kToken;
        auto writer = std::make_shared<CsrfMockResponseWriter>();
        EXPECT_FALSE(csrf(req, writer)) << static_cast<int>(m);
        expect_rejected(*writer);
    }
}

TEST(CsrfTest, MatchingHeaderTokenIsAccepted) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("X-CSRF-Token", kToken);
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_TRUE(csrf(req, writer));
    EXPECT_FALSE(writer->sent.has_value());
}

TEST(CsrfTest, HeaderNameIsCaseInsensitive) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("x-csrf-token", kToken);
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_TRUE(csrf(req, writer));
}

TEST(CsrfTest, MismatchedHeaderTokenIsRejected) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    std::string wrong = kToken;
    wrong.back() = '0';
    req.set_header("X-CSRF-Token", wrong);
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
    expect_rejected(*writer);
}

TEST(CsrfTest, TokenThatIsOnlyAPrefixIsRejected) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("X-CSRF-Token", kToken.substr(0, 32));
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
}

TEST(CsrfTest, MissingCookieIsRejectedEvenWithAToken) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.set_header("X-CSRF-Token", kToken);
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
    expect_rejected(*writer);
}

TEST(CsrfTest, MissingTokenIsRejected) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
    expect_rejected(*writer);
}

// An empty cookie and an empty header are equal strings, but not a token.
TEST(CsrfTest, EmptyCookieAndEmptyTokenAreRejected) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = "";
    req.set_header("X-CSRF-Token", "");
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
    expect_rejected(*writer);
}

TEST(CsrfTest, UrlEncodedFormTokenIsAccepted) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("Content-Type", "application/x-www-form-urlencoded");
    const std::string body = "title=hi&_csrf=" + kToken;
    req.body = body;
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_TRUE(csrf(req, writer));
}

TEST(CsrfTest, MultipartFormTokenIsAccepted) {
    Csrf csrf;
    auto post = [&](const std::string& token) {
        HttpRequest req = mutating();
        req.cookies["csrf_token"] = kToken;
        req.set_header("Content-Type", "multipart/form-data; boundary=XyZ");
        const std::string body =
            "--XyZ\r\n"
            "Content-Disposition: form-data; name=\"title\"\r\n\r\n"
            "hello\r\n"
            "--XyZ\r\n"
            "Content-Disposition: form-data; name=\"_csrf\"\r\n\r\n" +
            token + "\r\n"
            "--XyZ--\r\n";
        req.body = body;
        auto writer = std::make_shared<CsrfMockResponseWriter>();
        return csrf(req, writer);
    };
    EXPECT_TRUE(post(kToken));
    EXPECT_FALSE(post("not-the-token"));
}

TEST(CsrfTest, FormTokenIsIgnoredForOtherContentTypes) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("Content-Type", "text/plain");
    const std::string body = "_csrf=" + kToken;
    req.body = body;
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
}

// A header, when present, is the token checked; a correct form field does
// not rescue a wrong header.
TEST(CsrfTest, HeaderTakesPrecedenceOverTheForm) {
    Csrf csrf;
    HttpRequest req = mutating();
    req.cookies["csrf_token"] = kToken;
    req.set_header("X-CSRF-Token", "wrong");
    req.set_header("Content-Type", "application/x-www-form-urlencoded");
    const std::string body = "_csrf=" + kToken;
    req.body = body;
    auto writer = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(csrf(req, writer));
}

TEST(CsrfTest, CustomCookieAndHeaderNames) {
    auto m = csrf_protection("xsrf", "X-XSRF-Token");

    HttpRequest get;
    get.method = HttpMethod::GET;
    auto w1 = std::make_shared<CsrfMockResponseWriter>();
    ASSERT_TRUE(m(get, w1));
    HttpResponse issued = w1->intercepted();
    ASSERT_EQ(issued.cookies.size(), 1u);
    EXPECT_EQ(issued.cookies[0].name, "xsrf");
    EXPECT_EQ(get.headers["X-XSRF-Token"], issued.cookies[0].value);

    HttpRequest post = mutating();
    post.cookies["xsrf"] = kToken;
    post.set_header("X-XSRF-Token", kToken);
    auto w2 = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_TRUE(m(post, w2));

    // The default names mean nothing to this instance.
    HttpRequest defaults = mutating();
    defaults.cookies["csrf_token"] = kToken;
    defaults.set_header("X-CSRF-Token", kToken);
    auto w3 = std::make_shared<CsrfMockResponseWriter>();
    EXPECT_FALSE(m(defaults, w3));
}
