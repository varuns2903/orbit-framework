#pragma once
#include <orbit/routing/Router.hpp>
#include <chrono>
#include <string>

namespace middleware {

/**
 * @brief Verification settings for jwt_auth().
 */
struct JwtOptions {
    std::string secret;                          ///< HMAC-SHA256 key; must not be empty
    std::string issuer;                          ///< If set, the "iss" claim must equal it
    std::string audience;                        ///< If set, "aud" (string or array) must contain it
    bool require_exp = false;                    ///< Reject tokens without an "exp" claim
    std::chrono::seconds leeway{0};              ///< Clock skew tolerated for "exp" and "nbf"
};

/**
 * @ingroup middlewares
 * @brief Verifies an HS256 `Authorization: Bearer` token and stores its claims in `req.user`.
 *
 * Only tokens whose header declares `"alg": "HS256"` are accepted. The
 * signature is compared in constant time; `exp` and `nbf` are enforced when
 * present (and `exp` is required when `require_exp` is set).
 *
 * @throws std::invalid_argument if the secret is empty.
 */
routing::Middleware jwt_auth(JwtOptions options);

/**
 * @ingroup middlewares
 * @brief Shorthand for jwt_auth(JwtOptions{secret_key}).
 */
routing::Middleware jwt_auth(const std::string& secret_key);

} // namespace middleware
