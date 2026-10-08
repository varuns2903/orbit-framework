#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/routing/Router.hpp>
#include <chrono>
#include <string>

namespace orbit::middleware {

/**
 * @brief Verification settings for jwt_auth(). Configure at least one key:
 *        `secret`, `public_key_pem` or `jwks_url` (several may be combined).
 */
struct JwtOptions {
    std::string secret;                          ///< HS256: HMAC-SHA256 key
    /// RS256 or ES256: a PEM public key ("-----BEGIN PUBLIC KEY-----").
    /// The key type decides the algorithm: RSA (>= 2048 bits) means RS256,
    /// EC P-256 means ES256.
    std::string public_key_pem;
    /// RS256/ES256 keys published as a JSON Web Key Set, e.g.
    /// "https://issuer.example.com/.well-known/jwks.json". The token's "kid"
    /// selects the key; an unknown kid triggers a refetch (key rotation).
    std::string jwks_url;
    std::chrono::seconds jwks_refresh{3600};     ///< Refetch the key set at least this often
    std::chrono::seconds jwks_min_refetch{30};   ///< Minimum gap between fetches (bounds load from bogus kids)
    std::chrono::seconds jwks_timeout{5};        ///< Timeout for one JWKS request

    std::string issuer;                          ///< If set, the "iss" claim must equal it
    std::string audience;                        ///< If set, "aud" (string or array) must contain it
    bool require_exp = false;                    ///< Reject tokens without an "exp" claim
    std::chrono::seconds leeway{0};              ///< Clock skew tolerated for "exp" and "nbf"
};

/**
 * @ingroup middlewares
 * @brief Verifies an `Authorization: Bearer` JWT and stores its claims in `req.user`.
 *
 * Supported algorithms are HS256, RS256 and ES256. Every key has exactly one
 * algorithm, and the token's "alg" must match the key it is verified with, so
 * a token cannot switch algorithms (e.g. present an RSA public key as an HMAC
 * secret, or use "none"). HMAC signatures are compared in constant time;
 * `exp` and `nbf` are enforced when present (and `exp` is required when
 * `require_exp` is set).
 *
 * @throws std::invalid_argument if no key is configured or a key is unusable.
 */
routing::Middleware jwt_auth(JwtOptions options);

/**
 * @ingroup middlewares
 * @brief Shorthand for an HS256-only jwt_auth() with this secret.
 */
routing::Middleware jwt_auth(const std::string& secret_key);

} // namespace middleware
