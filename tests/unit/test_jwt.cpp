#include <gtest/gtest.h>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <chrono>
#include <memory>
#include <stdexcept>

using namespace orbit::http;

namespace {

const std::string kSecret = "0123456789abcdef0123456789abcdef";

std::string b64url(const std::string& in) {
    std::string out(4 * ((in.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
    out.resize(static_cast<size_t>(n));
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

std::string sign(const std::string& header_json, const std::string& payload_json, const std::string& secret = kSecret) {
    std::string data = b64url(header_json) + "." + b64url(payload_json);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac, &len);
    return data + "." + b64url(std::string(reinterpret_cast<char*>(mac), len));
}

std::string hs256(const std::string& payload_json) {
    return sign(R"({"alg":"HS256","typ":"JWT"})", payload_json);
}

long long now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class JwtTestWriter : public ResponseWriter {
public:
    HttpResponse last;
    void send(HttpResponse&& r) override { last = std::move(r); }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

// Returns true if the middleware let the request through.
bool check(const orbit::routing::Middleware& mw, const std::string& token, std::string* body = nullptr, HttpRequest* out = nullptr) {
    HttpRequest req;
    std::string header = "Bearer " + token;
    req.headers["Authorization"] = header;
    auto w = std::make_shared<JwtTestWriter>();
    bool ok = mw(req, w);
    if (body) *body = w->last.body;
    if (!ok) EXPECT_EQ(w->last.headers["WWW-Authenticate"], "Bearer");
    if (out) *out = std::move(req);
    return ok;
}

} // namespace

TEST(JwtAuthTest, AcceptsValidTokenAndExposesClaims) {
    auto mw = orbit::middleware::jwt_auth(kSecret);
    HttpRequest req;
    ASSERT_TRUE(check(mw, hs256(R"({"sub":"alice","exp":)" + std::to_string(now() + 60) + "}"), nullptr, &req));
    EXPECT_EQ(req.user["sub"], "alice");
}

TEST(JwtAuthTest, RejectsWrongSignature) {
    auto mw = orbit::middleware::jwt_auth(kSecret);
    std::string forged = sign(R"({"alg":"HS256"})", R"({"sub":"alice"})", "a-different-secret-of-some-length!!");
    EXPECT_FALSE(check(mw, forged));
}

TEST(JwtAuthTest, RejectsAlgorithmsOtherThanHs256) {
    auto mw = orbit::middleware::jwt_auth(kSecret);
    std::string body;
    // Correct HMAC, but the header claims another algorithm.
    EXPECT_FALSE(check(mw, sign(R"({"alg":"none"})", R"({"sub":"alice"})"), &body));
    EXPECT_NE(body.find("algorithm"), std::string::npos);
    EXPECT_FALSE(check(mw, sign(R"({"alg":"HS512"})", R"({"sub":"alice"})")));
    EXPECT_FALSE(check(mw, sign(R"({"typ":"JWT"})", R"({"sub":"alice"})")));
    EXPECT_FALSE(check(mw, sign("not json", R"({"sub":"alice"})")));
}

TEST(JwtAuthTest, EnforcesExpAndNbfWithLeeway) {
    auto strict = orbit::middleware::jwt_auth(kSecret);
    EXPECT_FALSE(check(strict, hs256(R"({"exp":)" + std::to_string(now() - 30) + "}")));
    EXPECT_FALSE(check(strict, hs256(R"({"nbf":)" + std::to_string(now() + 30) + "}")));
    EXPECT_FALSE(check(strict, hs256(R"({"exp":"tomorrow"})")));

    orbit::middleware::JwtOptions opts;
    opts.secret = kSecret;
    opts.leeway = std::chrono::seconds(60);
    auto lenient = orbit::middleware::jwt_auth(opts);
    EXPECT_TRUE(check(lenient, hs256(R"({"exp":)" + std::to_string(now() - 30) + "}")));
    EXPECT_TRUE(check(lenient, hs256(R"({"nbf":)" + std::to_string(now() + 30) + "}")));
}

TEST(JwtAuthTest, RequireExpRejectsNonExpiringTokens) {
    orbit::middleware::JwtOptions opts;
    opts.secret = kSecret;
    opts.require_exp = true;
    auto mw = orbit::middleware::jwt_auth(opts);
    EXPECT_FALSE(check(mw, hs256(R"({"sub":"alice"})")));
    EXPECT_TRUE(check(mw, hs256(R"({"exp":)" + std::to_string(now() + 60) + "}")));
}

TEST(JwtAuthTest, ChecksIssuerAndAudience) {
    orbit::middleware::JwtOptions opts;
    opts.secret = kSecret;
    opts.issuer = "https://auth.example";
    opts.audience = "orders-api";
    auto mw = orbit::middleware::jwt_auth(opts);
    EXPECT_TRUE(check(mw, hs256(R"({"iss":"https://auth.example","aud":"orders-api"})")));
    EXPECT_TRUE(check(mw, hs256(R"({"iss":"https://auth.example","aud":["x","orders-api"]})")));
    EXPECT_FALSE(check(mw, hs256(R"({"iss":"https://evil.example","aud":"orders-api"})")));
    EXPECT_FALSE(check(mw, hs256(R"({"iss":"https://auth.example","aud":"billing-api"})")));
    EXPECT_FALSE(check(mw, hs256(R"({"iss":"https://auth.example"})")));
}

TEST(JwtAuthTest, RejectsMalformedTokens) {
    auto mw = orbit::middleware::jwt_auth(kSecret);
    EXPECT_FALSE(check(mw, "abc"));
    EXPECT_FALSE(check(mw, "a.b"));
    EXPECT_FALSE(check(mw, hs256(R"({"sub":"a"})") + ".extra"));
    EXPECT_FALSE(check(mw, hs256(R"([1,2,3])")));
}

TEST(JwtAuthTest, EmptySecretIsRefused) {
    EXPECT_THROW(orbit::middleware::jwt_auth(std::string()), std::invalid_argument);
}
