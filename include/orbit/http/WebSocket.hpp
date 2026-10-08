#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <string_view>
#include <openssl/sha.h>
#include <openssl/evp.h>

namespace orbit::http {
namespace websocket {

/**
 * @brief Utility class for WebSocket handshakes.
 */
class Handshake {
public:
    /**
     * @brief Checks that a Sec-WebSocket-Key is the base64 encoding of 16 bytes
     *        (RFC 6455 section 4.1): 22 base64 characters followed by "==".
     */
    static bool is_valid_client_key(std::string_view key) {
        if (key.size() != 24 || key.substr(22) != "==") return false;
        for (size_t i = 0; i < 22; ++i) {
            char c = key[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
            if (!ok) return false;
        }
        return true;
    }

    /**
     * @brief Generates the Sec-WebSocket-Accept key for the WebSocket handshake.
     * @param client_key The base64-encoded Sec-WebSocket-Key from the client request.
     * @return The base64-encoded response key to send back to the client.
     */
    static std::string generate_accept_key(const std::string& client_key) {
        const std::string magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string combined = client_key + magic_string;
        
        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(combined.c_str()), combined.length(), hash);
        
        // Base64 encode using OpenSSL EVP
        int expected_len = 4 * ((SHA_DIGEST_LENGTH + 2) / 3);
        std::string base64_key(static_cast<size_t>(expected_len), '\0');
        
        int encoded_len = EVP_EncodeBlock(
            reinterpret_cast<unsigned char*>(&base64_key[0]),
            hash,
            SHA_DIGEST_LENGTH
        );
        
        base64_key.resize(static_cast<size_t>(encoded_len));
        return base64_key;
    }
};

} // namespace websocket
} // namespace http
