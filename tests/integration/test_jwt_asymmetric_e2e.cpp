#include <gtest/gtest.h>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

using namespace http;

namespace {

using PKey = std::shared_ptr<EVP_PKEY>;

std::string b64url(const std::string& in) {
    std::string out(4 * ((in.size() + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
    out.resize(static_cast<size_t>(n));
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

PKey rsa_key(int bits) { return PKey(EVP_RSA_gen(static_cast<unsigned>(bits)), EVP_PKEY_free); }
PKey ec_key() { return PKey(EVP_EC_gen("P-256"), EVP_PKEY_free); }

std::string public_pem(const PKey& key) {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, key.get());
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(len));
    BIO_free(bio);
    return pem;
}

std::string bn_bytes(const PKey& key, const char* name) {
    BIGNUM* bn = nullptr;
    EVP_PKEY_get_bn_param(key.get(), name, &bn);
    std::string out(static_cast<size_t>(BN_num_bytes(bn)), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(&out[0]));
    BN_free(bn);
    return out;
}

nlohmann::json jwk(const PKey& key, const std::string& kid) {
    if (EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_RSA) {
        return {{"kty", "RSA"}, {"kid", kid}, {"use", "sig"}, {"alg", "RS256"},
                {"n", b64url(bn_bytes(key, OSSL_PKEY_PARAM_RSA_N))}, {"e", b64url(bn_bytes(key, OSSL_PKEY_PARAM_RSA_E))}};
    }
    unsigned char point[65];
    size_t len = 0;
    EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &len);
    std::string p(reinterpret_cast<char*>(point), len); // 0x04 || x || y
    return {{"kty", "EC"}, {"kid", kid}, {"crv", "P-256"}, {"x", b64url(p.substr(1, 32))}, {"y", b64url(p.substr(33, 32))}};
}

std::string sign(const PKey& key, const std::string& alg, const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key.get());
    size_t len = 0;
    EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    std::string sig(len, '\0');
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(&sig[0]), &len,
                   reinterpret_cast<const unsigned char*>(data.data()), data.size());
    sig.resize(len);
    EVP_MD_CTX_free(ctx);
    if (alg == "ES256") { // DER -> raw r || s
        const unsigned char* p = reinterpret_cast<const unsigned char*>(sig.data());
        ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()));
        std::string raw(64, '\0');
        BN_bn2binpad(ECDSA_SIG_get0_r(es), reinterpret_cast<unsigned char*>(&raw[0]), 32);
        BN_bn2binpad(ECDSA_SIG_get0_s(es), reinterpret_cast<unsigned char*>(&raw[32]), 32);
        ECDSA_SIG_free(es);
        return raw;
    }
    return sig;
}

std::string make_token(const std::string& alg, const nlohmann::json& claims, const std::string& kid,
                       const std::function<std::string(const std::string&)>& signer) {
    nlohmann::json header = {{"alg", alg}, {"typ", "JWT"}};
    if (!kid.empty()) header["kid"] = kid;
    std::string signed_part = b64url(header.dump()) + "." + b64url(claims.dump());
    return signed_part + "." + b64url(signer(signed_part));
}

std::string token_for(const PKey& key, const std::string& alg, const std::string& kid = "") {
    return make_token(alg, {{"sub", "alice"}}, kid, [&](const std::string& d) { return sign(key, alg, d); });
}

class StatusWriter : public ResponseWriter {
public:
    int status = 0;
    std::string body;
    void send(HttpResponse&& r) override {
        status = static_cast<int>(r.status_code);
        body = r.body;
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

// Runs the middleware; returns "" if accepted, otherwise the error body.
std::string check(const routing::Middleware& mw, const std::string& token) {
    HttpRequest req;
    std::string auth = "Bearer " + token;
    req.set_header("Authorization", auth);
    auto w = std::make_shared<StatusWriter>();
    if (mw(req, w)) {
        EXPECT_EQ(req.user.value("sub", ""), "alice");
        return "";
    }
    EXPECT_EQ(w->status, 401);
    return w->body.empty() ? "rejected" : w->body;
}

} // namespace

TEST(JwtAsymmetricTest, Rs256WithPemKey) {
    PKey key = rsa_key(2048);
    middleware::JwtOptions opts;
    opts.public_key_pem = public_pem(key);
    auto mw = middleware::jwt_auth(opts);

    EXPECT_EQ(check(mw, token_for(key, "RS256")), "");
    // Signed by somebody else's key.
    EXPECT_NE(check(mw, token_for(rsa_key(2048), "RS256")), "");
    // Payload altered after signing.
    std::string t = token_for(key, "RS256");
    size_t dot = t.find('.');
    std::string forged = t.substr(0, dot + 1) + b64url(R"({"sub":"alice","admin":true})") + t.substr(t.find('.', dot + 1));
    EXPECT_NE(check(mw, forged), "");
}

TEST(JwtAsymmetricTest, Es256WithPemKey) {
    PKey key = ec_key();
    middleware::JwtOptions opts;
    opts.public_key_pem = public_pem(key);
    auto mw = middleware::jwt_auth(opts);
    EXPECT_EQ(check(mw, token_for(key, "ES256")), "");
    EXPECT_NE(check(mw, token_for(ec_key(), "ES256")), "");
    // DER instead of raw r||s is not a valid JWS signature.
    EXPECT_NE(check(mw, make_token("ES256", {{"sub", "alice"}}, "", [](const std::string&) { return std::string(70, 'x'); })), "");
}

TEST(JwtAsymmetricTest, TokensCannotChooseTheAlgorithm) {
    PKey rsa = rsa_key(2048);
    std::string pem = public_pem(rsa);
    middleware::JwtOptions opts;
    opts.public_key_pem = pem;
    auto mw = middleware::jwt_auth(opts);

    // Classic confusion: HS256 "signed" with the public key text as the HMAC secret.
    auto hmac_with_pem = [&pem](const std::string& data) {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), pem.data(), static_cast<int>(pem.size()),
             reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
        return std::string(reinterpret_cast<char*>(out), len);
    };
    EXPECT_NE(check(mw, make_token("HS256", {{"sub", "alice"}}, "", hmac_with_pem)), "");
    // An RSA key never verifies ES256, and "none" is never accepted.
    EXPECT_NE(check(mw, token_for(ec_key(), "ES256")), "");
    EXPECT_NE(check(mw, make_token("none", {{"sub", "alice"}}, "", [](const std::string&) { return std::string(); })), "");
}

TEST(JwtAsymmetricTest, UnusableKeysAreRefusedAtSetup) {
    middleware::JwtOptions weak;
    weak.public_key_pem = public_pem(rsa_key(1024));
    EXPECT_THROW(middleware::jwt_auth(weak), std::invalid_argument);

    middleware::JwtOptions garbage;
    garbage.public_key_pem = "-----BEGIN PUBLIC KEY-----\nnope\n-----END PUBLIC KEY-----\n";
    EXPECT_THROW(middleware::jwt_auth(garbage), std::invalid_argument);

    EXPECT_THROW(middleware::jwt_auth(middleware::JwtOptions{}), std::invalid_argument);
}

TEST(JwtAsymmetricTest, JwksSelectsKeysByKidAndFollowsRotation) {
    PKey k1 = rsa_key(2048);
    PKey k2 = ec_key();
    std::mutex mutex;
    nlohmann::json jwks = {{"keys", nlohmann::json::array({jwk(k1, "k1")})}};

    config::ServerConfig cfg = orbit::test::server_config();
    cfg.port = 8128;
    server::App issuer(cfg);
    issuer.get("/jwks.json", [&](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        {
            std::lock_guard<std::mutex> lock(mutex);
            res.set_body(jwks.dump(), "application/json");
        }
        w->send(std::move(res));
    });
    std::thread server([&issuer] { issuer.listen(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    middleware::JwtOptions opts;
    opts.jwks_url = "http://127.0.0.1:8128/jwks.json";
    opts.jwks_min_refetch = std::chrono::seconds(0); // let the test rotate immediately
    auto mw = middleware::jwt_auth(opts);

    EXPECT_EQ(check(mw, token_for(k1, "RS256", "k1")), "");
    EXPECT_NE(check(mw, token_for(k2, "ES256", "k2")), ""); // not published yet

    {
        std::lock_guard<std::mutex> lock(mutex);
        jwks = {{"keys", nlohmann::json::array({jwk(k2, "k2")})}}; // rotate: k1 retired
    }
    EXPECT_EQ(check(mw, token_for(k2, "ES256", "k2")), "");
    EXPECT_NE(check(mw, token_for(k1, "RS256", "k1")), "");
    // A key from the set cannot be used with another algorithm.
    EXPECT_NE(check(mw, make_token("RS256", {{"sub", "alice"}}, "k2", [&](const std::string& d) { return sign(k2, "ES256", d); })), "");

    issuer.stop();
    server.join();
}
