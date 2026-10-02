#pragma once
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace utils {

/**
 * @brief Returns @p num_bytes of cryptographically secure randomness, hex-encoded.
 *
 * Use this for anything an attacker must not guess: session identifiers,
 * CSRF tokens, OAuth state values. std::random_device / std::mt19937 are not
 * suitable for these.
 *
 * @throws std::runtime_error if the system CSPRNG fails.
 */
inline std::string secure_random_hex(size_t num_bytes) {
    std::vector<unsigned char> bytes(num_bytes);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(num_bytes * 2);
    for (unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

/**
 * @brief Compares two secrets in time independent of where they differ.
 */
inline bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace utils
