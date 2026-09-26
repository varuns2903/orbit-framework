#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <orbit/utils/Logger.hpp>
#include <stdexcept>
#include <vector>
#include <chrono>

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

} // namespace

routing::Middleware jwt_auth(JwtOptions options) {
    if (options.secret.empty()) {
        throw std::invalid_argument("jwt_auth: the secret must not be empty");
    }
    if (options.secret.size() < 32) {
        LOG_WARN("jwt_auth: secret is shorter than 32 bytes; HS256 keys should have at least 256 bits of entropy");
    }

    return [options](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
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

        // The algorithm is fixed by the server, never chosen by the token.
        nlohmann::json header = nlohmann::json::parse(base64url_decode(header_b64), nullptr, false);
        if (header.is_discarded() || !header.is_object() || !header.contains("alg") ||
            !header["alg"].is_string() || header["alg"].get<std::string>() != "HS256") {
            reject(writer, R"({"error": "Unsupported JWT algorithm"})");
            return false;
        }
        
        if (!verify_jwt_signature(header_b64, payload_b64, signature_b64, options.secret)) {
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
    return jwt_auth(std::move(options));
}

} // namespace middleware
