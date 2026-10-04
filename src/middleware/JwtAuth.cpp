#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <curl/curl.h>
#include <orbit/utils/Logger.hpp>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace middleware {

static std::string base64url_encode(const unsigned char* input, int length) {
    int out_len = 4 * ((length + 2) / 3);
    std::vector<unsigned char> out(out_len + 1);
    EVP_EncodeBlock(out.data(), input, length);
    
    std::string b64(reinterpret_cast<char*>(out.data()));
    // Remove padding
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    // Replace characters
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return b64;
}

static std::string base64url_decode(const std::string& input) {
    std::string b64 = input;
    // Replace characters
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Add padding
    while (b64.size() % 4 != 0) {
        b64 += '=';
    }
    
    size_t out_len = (b64.size() * 3) / 4;
    std::vector<unsigned char> out(out_len + 1);
    
    int dec_len = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(b64.data()), b64.size());
    if (dec_len < 0) return "";
    
    int padding = 0;
    if (b64.length() > 0 && b64[b64.length() - 1] == '=') padding++;
    if (b64.length() > 1 && b64[b64.length() - 2] == '=') padding++;
    
    return std::string(reinterpret_cast<char*>(out.data()), dec_len - padding);
}

static bool verify_jwt_signature(const std::string& header_b64, const std::string& payload_b64, const std::string& provided_signature, const std::string& secret) {
    std::string data_to_sign = header_b64 + "." + payload_b64;
    
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.length()), 
         reinterpret_cast<const unsigned char*>(data_to_sign.data()), data_to_sign.length(), 
         hash, &hash_len);
         
    std::string expected_signature = base64url_encode(hash, static_cast<int>(hash_len));
    // Constant-time: a plain == leaks how many leading characters matched.
    return provided_signature.size() == expected_signature.size() &&
           CRYPTO_memcmp(provided_signature.data(), expected_signature.data(), expected_signature.size()) == 0;
}

namespace {

void reject(const std::shared_ptr<http::ResponseWriter>& writer, const char* json_error) {
    http::HttpResponse res;
    res.status(http::HttpStatus::Unauthorized).json(std::string(json_error));
    res.headers["WWW-Authenticate"] = "Bearer";
    writer->send(std::move(res));
}

bool audience_matches(const nlohmann::json& aud, const std::string& expected) {
    if (aud.is_string()) return aud.get<std::string>() == expected;
    if (aud.is_array()) {
        for (const auto& a : aud) {
            if (a.is_string() && a.get<std::string>() == expected) return true;
        }
    }
    return false;
}

// --- Public keys (RS256 / ES256) ---

using PKey = std::shared_ptr<EVP_PKEY>;

PKey wrap(EVP_PKEY* raw) {
    return PKey(raw, EVP_PKEY_free);
}

// The one algorithm a key may verify, or "" if the key is unsuitable.
std::string algorithm_for(EVP_PKEY* key) {
    switch (EVP_PKEY_get_base_id(key)) {
        case EVP_PKEY_RSA:
            return EVP_PKEY_get_bits(key) >= 2048 ? "RS256" : "";
        case EVP_PKEY_EC: {
            char group[64] = {};
            size_t len = 0;
            if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &len) != 1) return "";
            return std::string(group) == "prime256v1" ? "ES256" : "";
        }
        default:
            return "";
    }
}

struct PublicKey {
    std::string alg;
    PKey key;
};

std::optional<PublicKey> key_from_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return std::nullopt;
    EVP_PKEY* raw = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!raw) return std::nullopt;
    PKey key = wrap(raw);
    std::string alg = algorithm_for(key.get());
    if (alg.empty()) return std::nullopt;
    return PublicKey{alg, key};
}

std::optional<PublicKey> key_from_jwk(const nlohmann::json& jwk) {
    if (!jwk.is_object() || !jwk.contains("kty") || !jwk["kty"].is_string()) return std::nullopt;
    if (jwk.contains("use") && jwk["use"] != "sig") return std::nullopt;
    auto field = [&jwk](const char* name) -> std::string {
        return jwk.contains(name) && jwk[name].is_string() ? base64url_decode(jwk[name].get<std::string>()) : "";
    };
    const std::string kty = jwk["kty"].get<std::string>();

    OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
    if (!bld) return std::nullopt;
    const char* type = nullptr;
    BIGNUM* n = nullptr;
    BIGNUM* e = nullptr;
    std::string point;
    bool ok = false;
    if (kty == "RSA") {
        std::string nb = field("n"), eb = field("e");
        if (!nb.empty() && !eb.empty()) {
            n = BN_bin2bn(reinterpret_cast<const unsigned char*>(nb.data()), static_cast<int>(nb.size()), nullptr);
            e = BN_bin2bn(reinterpret_cast<const unsigned char*>(eb.data()), static_cast<int>(eb.size()), nullptr);
            ok = n && e && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) &&
                 OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e);
            type = "RSA";
        }
    } else if (kty == "EC" && jwk.value("crv", "") == "P-256") {
        std::string x = field("x"), y = field("y");
        if (x.size() == 32 && y.size() == 32) {
            point = std::string(1, '\x04') + x + y; // uncompressed point
            ok = OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) &&
                 OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
            type = "EC";
        }
    }

    EVP_PKEY* raw = nullptr;
    if (ok) {
        OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr);
        if (params && ctx && EVP_PKEY_fromdata_init(ctx) == 1) {
            EVP_PKEY_fromdata(ctx, &raw, EVP_PKEY_PUBLIC_KEY, params);
        }
        EVP_PKEY_CTX_free(ctx);
        OSSL_PARAM_free(params);
    }
    OSSL_PARAM_BLD_free(bld);
    BN_free(n);
    BN_free(e);
    if (!raw) return std::nullopt;

    PKey key = wrap(raw);
    std::string alg = algorithm_for(key.get());
    if (alg.empty()) return std::nullopt;
    // A JWK may pin its algorithm; it must agree with the key type.
    if (jwk.contains("alg") && jwk["alg"] != alg) return std::nullopt;
    return PublicKey{alg, key};
}

bool verify_with_key(const PublicKey& key, const std::string& signed_part, const std::string& signature) {
    std::string der;
    const std::string* sig = &signature;
    if (key.alg == "ES256") {
        // JWS carries ECDSA as raw r || s (32 bytes each); OpenSSL wants DER.
        if (signature.size() != 64) return false;
        ECDSA_SIG* es = ECDSA_SIG_new();
        BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()), 32, nullptr);
        BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(signature.data()) + 32, 32, nullptr);
        if (!es || !r || !s || ECDSA_SIG_set0(es, r, s) != 1) {
            BN_free(r);
            BN_free(s);
            ECDSA_SIG_free(es);
            return false;
        }
        unsigned char* out = nullptr;
        int len = i2d_ECDSA_SIG(es, &out);
        ECDSA_SIG_free(es);
        if (len <= 0) return false;
        der.assign(reinterpret_cast<char*>(out), static_cast<size_t>(len));
        OPENSSL_free(out);
        sig = &der;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key.key.get()) == 1 &&
              EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig->data()), sig->size(),
                               reinterpret_cast<const unsigned char*>(signed_part.data()), signed_part.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

// --- JWKS ---

size_t collect(void* data, size_t size, size_t n, void* out) {
    auto* body = static_cast<std::string*>(out);
    if (body->size() + size * n > 1024 * 1024) return 0; // a key set is small; refuse anything huge
    body->append(static_cast<char*>(data), size * n);
    return size * n;
}

class JwksCache {
public:
    JwksCache(std::string url, std::chrono::seconds refresh, std::chrono::seconds min_refetch, std::chrono::seconds timeout)
        : url_(std::move(url)), refresh_(refresh), min_refetch_(min_refetch), timeout_(timeout) {}

    // The key for `kid` (or the only key when the token names none).
    std::optional<PublicKey> find(const std::string& kid) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();
        bool stale = !fetched_ || now - fetched_at_ >= refresh_;
        bool unknown = !lookup_locked(kid);
        // Refetch when stale or for an unknown kid (key rotation), but never
        // more often than min_refetch: a stream of bogus kids must not turn
        // into a stream of requests to the issuer.
        if ((stale || unknown) && (!attempted_ || now - attempted_at_ >= min_refetch_)) {
            fetch_locked();
        }
        return lookup_locked(kid);
    }

private:
    using Clock = std::chrono::steady_clock;

    std::optional<PublicKey> lookup_locked(const std::string& kid) const {
        if (kid.empty()) {
            if (keys_.size() == 1) return keys_.begin()->second;
            return std::nullopt;
        }
        auto it = keys_.find(kid);
        if (it == keys_.end()) return std::nullopt;
        return it->second;
    }

    void fetch_locked() {
        attempted_ = true;
        attempted_at_ = Clock::now();
        static std::once_flag curl_init;
        std::call_once(curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        CURL* curl = curl_easy_init();
        if (!curl) return;
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_.count()));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS | CURLPROTO_HTTP));
#endif
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK || status != 200) {
            LOG_WARN("jwt_auth: fetching JWKS from " << url_ << " failed: "
                     << (rc != CURLE_OK ? curl_easy_strerror(rc) : ("HTTP " + std::to_string(status)).c_str()));
            return; // keep the keys we had
        }

        auto json = nlohmann::json::parse(body, nullptr, false);
        if (!json.is_object() || !json.contains("keys") || !json["keys"].is_array()) {
            LOG_WARN("jwt_auth: " << url_ << " did not return a JSON Web Key Set");
            return;
        }
        std::unordered_map<std::string, PublicKey> keys;
        for (const auto& jwk : json["keys"]) {
            auto key = key_from_jwk(jwk);
            if (!key) continue; // unsupported or unusable keys are skipped
            std::string kid = jwk.contains("kid") && jwk["kid"].is_string() ? jwk["kid"].get<std::string>() : "";
            keys[kid] = *key;
        }
        keys_ = std::move(keys);
        fetched_ = true;
        fetched_at_ = Clock::now();
    }

    std::string url_;
    std::chrono::seconds refresh_;
    std::chrono::seconds min_refetch_;
    std::chrono::seconds timeout_;
    std::mutex mutex_;
    std::unordered_map<std::string, PublicKey> keys_;
    bool fetched_ = false;
    bool attempted_ = false;
    Clock::time_point fetched_at_{};
    Clock::time_point attempted_at_{};
};

} // namespace

routing::Middleware jwt_auth(JwtOptions options) {
    if (options.secret.empty() && options.public_key_pem.empty() && options.jwks_url.empty()) {
        throw std::invalid_argument("jwt_auth: configure a secret, public_key_pem or jwks_url");
    }
    if (!options.secret.empty() && options.secret.size() < 32) {
        LOG_WARN("jwt_auth: secret is shorter than 32 bytes; HS256 keys should have at least 256 bits of entropy");
    }
    std::optional<PublicKey> static_key;
    if (!options.public_key_pem.empty()) {
        static_key = key_from_pem(options.public_key_pem);
        if (!static_key) {
            throw std::invalid_argument("jwt_auth: public_key_pem must be an RSA (>= 2048 bits) or EC P-256 public key");
        }
    }
    std::shared_ptr<JwksCache> jwks;
    if (!options.jwks_url.empty()) {
        jwks = std::make_shared<JwksCache>(options.jwks_url, options.jwks_refresh, options.jwks_min_refetch,
                                           options.jwks_timeout);
    }

    return [options, static_key, jwks](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        auto auth_it = req.headers.find("Authorization");
        if (auth_it == req.headers.end()) {
            reject(writer, R"({"error": "Missing Authorization header"})");
            return false;
        }
        
        std::string_view auth_header = auth_it->second;
        if (!auth_header.starts_with("Bearer ")) {
            reject(writer, R"({"error": "Invalid Authorization scheme"})");
            return false;
        }
        
        std::string token(auth_header.substr(7));
        
        // Split token by '.'
        size_t first_dot = token.find('.');
        size_t second_dot = first_dot == std::string::npos ? std::string::npos : token.find('.', first_dot + 1);
        
        if (first_dot == std::string::npos || second_dot == std::string::npos ||
            token.find('.', second_dot + 1) != std::string::npos) {
            reject(writer, R"({"error": "Malformed JWT"})");
            return false;
        }
        
        std::string header_b64 = token.substr(0, first_dot);
        std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
        std::string signature_b64 = token.substr(second_dot + 1);

        nlohmann::json header = nlohmann::json::parse(base64url_decode(header_b64), nullptr, false);
        if (header.is_discarded() || !header.is_object() || !header.contains("alg") || !header["alg"].is_string()) {
            reject(writer, R"({"error": "Unsupported JWT algorithm"})");
            return false;
        }
        const std::string alg = header["alg"].get<std::string>();
        const std::string kid = header.contains("kid") && header["kid"].is_string() ? header["kid"].get<std::string>() : "";

        // The key decides the algorithm; the token only has to agree with it.
        bool verified = false;
        if (alg == "HS256") {
            if (options.secret.empty()) {
                reject(writer, R"({"error": "Unsupported JWT algorithm"})");
                return false;
            }
            verified = verify_jwt_signature(header_b64, payload_b64, signature_b64, options.secret);
        } else if (alg == "RS256" || alg == "ES256") {
            std::optional<PublicKey> key;
            if (jwks && (!kid.empty() || !static_key)) key = jwks->find(kid);
            if (!key && static_key) key = static_key;
            if (!key || key->alg != alg) {
                reject(writer, key ? R"({"error": "Unsupported JWT algorithm"})" : R"({"error": "Unknown signing key"})");
                return false;
            }
            verified = verify_with_key(*key, header_b64 + "." + payload_b64, base64url_decode(signature_b64));
        } else {
            reject(writer, R"({"error": "Unsupported JWT algorithm"})");
            return false;
        }
        if (!verified) {
            reject(writer, R"({"error": "Invalid JWT signature"})");
            return false;
        }
        
        nlohmann::json claims = nlohmann::json::parse(base64url_decode(payload_b64), nullptr, false);
        if (claims.is_discarded() || !claims.is_object()) {
            reject(writer, R"({"error": "Payload is not a JSON object"})");
            return false;
        }

        const long long now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const long long leeway = options.leeway.count();

        if (claims.contains("exp")) {
            if (!claims["exp"].is_number()) {
                reject(writer, R"({"error": "Invalid exp claim"})");
                return false;
            }
            if (now > claims["exp"].get<long long>() + leeway) {
                reject(writer, R"({"error": "Token expired"})");
                return false;
            }
        } else if (options.require_exp) {
            reject(writer, R"({"error": "Token has no expiry"})");
            return false;
        }

        if (claims.contains("nbf")) {
            if (!claims["nbf"].is_number()) {
                reject(writer, R"({"error": "Invalid nbf claim"})");
                return false;
            }
            if (now + leeway < claims["nbf"].get<long long>()) {
                reject(writer, R"({"error": "Token not yet valid"})");
                return false;
            }
        }

        if (!options.issuer.empty() &&
            (!claims.contains("iss") || !claims["iss"].is_string() || claims["iss"].get<std::string>() != options.issuer)) {
            reject(writer, R"({"error": "Invalid issuer"})");
            return false;
        }

        if (!options.audience.empty() && (!claims.contains("aud") || !audience_matches(claims["aud"], options.audience))) {
            reject(writer, R"({"error": "Invalid audience"})");
            return false;
        }

        req.user = std::move(claims);
        return true;
    };
}

routing::Middleware jwt_auth(const std::string& secret_key) {
    JwtOptions options;
    options.secret = secret_key;
    if (secret_key.empty()) {
        throw std::invalid_argument("jwt_auth: the secret must not be empty");
    }
    return jwt_auth(std::move(options));
}

} // namespace middleware
