#include <gtest/gtest.h>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

// orbit::jwt::sign() (#198): tokens it issues are accepted by jwt_auth()
// with the matching key, for HS256, RS256 and ES256.

using namespace orbit::http;

namespace {

const std::string kSecret = "0123456789abcdef0123456789abcdef";

class SignTestWriter : public ResponseWriter {
public:
    HttpResponse last;
    bool sent = false;
    void send(HttpResponse&& r) override {
        last = std::move(r);
        sent = true;
    }
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

// Runs @p mw on a request carrying @p token; returns the claims it accepted,
// or null if it rejected the token.
nlohmann::json verify(const orbit::routing::Middleware& mw, const std::string& token) {
    HttpRequest req;
    const std::string auth = "Bearer " + token;
    req.set_header("Authorization", auth);
    auto writer = std::make_shared<SignTestWriter>();
    if (!mw(req, writer)) return nullptr;
    return req.user;
}

using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

std::string pem(EVP_PKEY* key, bool private_part) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (private_part) {
        PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    } else {
        PEM_write_bio_PUBKEY(bio, key);
    }
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<size_t>(len));
    BIO_free(bio);
    return out;
}

PKey rsa_key(unsigned bits) { return PKey(EVP_RSA_gen(bits), EVP_PKEY_free); }
PKey ec_key(const char* curve) { return PKey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", curve), EVP_PKEY_free); }

long long now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

TEST(JwtSignTest, Hs256TokenIsAcceptedWithItsClaims) {
    std::string token = orbit::jwt::sign({{"role", "admin"}}, {.secret = kSecret},
                                         {.expires_in = std::chrono::hours(1), .issuer = "orbit",
                                          .audience = "app", .subject = "user-7"});
    orbit::middleware::JwtOptions opts;
    opts.secret = kSecret;
    opts.issuer = "orbit";
    opts.audience = "app";
    opts.require_exp = true;
    nlohmann::json claims = verify(orbit::middleware::jwt_auth(opts), token);
    ASSERT_TRUE(claims.is_object()) << token;
    EXPECT_EQ(claims["role"], "admin");
    EXPECT_EQ(orbit::jwt::claim<std::string>(claims, "sub"), "user-7");
    auto iat = orbit::jwt::claim<long long>(claims, "iat");
    auto exp = orbit::jwt::claim<long long>(claims, "exp");
    ASSERT_TRUE(iat && exp);
    EXPECT_LE(std::llabs(*iat - now()), 5);
    EXPECT_EQ(*exp - *iat, 3600);
}

TEST(JwtSignTest, WrongSecretIsRejected) {
    std::string token = orbit::jwt::sign({{"sub", "x"}}, {.secret = kSecret});
    std::string other = kSecret;
    other[0] = 'X';
    EXPECT_TRUE(verify(orbit::middleware::jwt_auth(other), token).is_null());
}

TEST(JwtSignTest, Rs256AndEs256TokensVerifyWithThePublicKey) {
    PKey keys[] = {rsa_key(2048), ec_key("P-256")};
    for (auto& key : keys) {
        std::string token = orbit::jwt::sign({{"sub", "k"}}, {.private_key_pem = pem(key.get(), true), .kid = "key-1"});
        orbit::middleware::JwtOptions opts;
        opts.public_key_pem = pem(key.get(), false);
        nlohmann::json claims = verify(orbit::middleware::jwt_auth(opts), token);
        ASSERT_TRUE(claims.is_object()) << token;
        EXPECT_EQ(claims["sub"], "k");

        // The header names the algorithm and the kid.
        std::string header_b64 = token.substr(0, token.find('.'));
        std::string padded = header_b64;
        for (char& c : padded) c = c == '-' ? '+' : c == '_' ? '/' : c;
        while (padded.size() % 4) padded += '=';
        std::string header(padded.size(), '\0');
        int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(header.data()),
                                reinterpret_cast<const unsigned char*>(padded.data()), static_cast<int>(padded.size()));
        header.resize(static_cast<size_t>(n));
        while (!header.empty() && header.back() == '\0') header.pop_back();
        auto h = nlohmann::json::parse(header);
        EXPECT_EQ(h["alg"], EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_RSA ? "RS256" : "ES256");
        EXPECT_EQ(h["kid"], "key-1");
    }
}

// Many ES256 signatures: r or s with a leading zero byte must still encode
// to exactly 32 bytes each.
TEST(JwtSignTest, Es256SignaturesAlwaysVerify) {
    auto key = ec_key("P-256");
    orbit::middleware::JwtOptions opts;
    opts.public_key_pem = pem(key.get(), false);
    auto mw = orbit::middleware::jwt_auth(opts);
    const std::string priv = pem(key.get(), true);
    for (int i = 0; i < 200; ++i) {
        std::string token = orbit::jwt::sign({{"n", i}}, {.private_key_pem = priv});
        ASSERT_TRUE(verify(mw, token).is_object()) << "signature " << i << " did not verify";
    }
}

TEST(JwtSignTest, ClaimsAlreadyPresentWin) {
    std::string token = orbit::jwt::sign({{"sub", "from-claims"}, {"iat", 1000}}, {.secret = kSecret},
                                         {.subject = "from-options"});
    nlohmann::json claims = verify(orbit::middleware::jwt_auth(kSecret), token);
    EXPECT_EQ(claims["sub"], "from-claims");
    EXPECT_EQ(claims["iat"], 1000);
}

TEST(JwtSignTest, NotBeforeInTheFutureIsRejectedUntilThen) {
    std::string token = orbit::jwt::sign({}, {.secret = kSecret}, {.not_before = std::chrono::hours(1)});
    EXPECT_TRUE(verify(orbit::middleware::jwt_auth(kSecret), token).is_null());
}

TEST(JwtSignTest, BadKeysAndClaimsThrow) {
    EXPECT_THROW(orbit::jwt::sign({}, {}), std::invalid_argument);
    EXPECT_THROW(orbit::jwt::sign({}, {.secret = kSecret, .private_key_pem = "x"}), std::invalid_argument);
    EXPECT_THROW(orbit::jwt::sign({}, {.private_key_pem = "not a pem"}), std::invalid_argument);
    EXPECT_THROW(orbit::jwt::sign({}, {.private_key_pem = pem(rsa_key(1024).get(), true)}), std::invalid_argument);
    EXPECT_THROW(orbit::jwt::sign({}, {.private_key_pem = pem(ec_key("P-384").get(), true)}), std::invalid_argument);
    EXPECT_THROW(orbit::jwt::sign(nlohmann::json::array(), {.secret = kSecret}), std::invalid_argument);
}

TEST(JwtSignTest, ClaimHelperChecksPresenceAndType) {
    nlohmann::json claims = {{"sub", "u"}, {"n", 3}};
    EXPECT_EQ(orbit::jwt::claim<std::string>(claims, "sub"), "u");
    EXPECT_EQ(orbit::jwt::claim<int>(claims, "n"), 3);
    EXPECT_FALSE(orbit::jwt::claim<std::string>(claims, "n").has_value()) << "wrong type";
    EXPECT_FALSE(orbit::jwt::claim<std::string>(claims, "missing").has_value());
    EXPECT_FALSE(orbit::jwt::claim<std::string>(nlohmann::json(), "sub").has_value());
}
