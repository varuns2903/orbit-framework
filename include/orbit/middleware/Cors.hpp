#pragma once
#include <orbit/routing/Router.hpp>
#include <string>
#include <vector>

namespace middleware {

/**
 * @ingroup middlewares
 * @brief Options for configuring CORS (Cross-Origin Resource Sharing).
 */
struct CorsOptions {
    std::vector<std::string> allowed_origins = {"*"};
    std::vector<std::string> allowed_methods = {"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    std::vector<std::string> allowed_headers = {"*"};
    bool allow_credentials = false;
    int max_age = 86400; // 24 hours default
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that handles CORS.
 *
 * @param options The CORS configuration options.
 * @return routing::Middleware The CORS middleware handler.
 */
routing::Middleware cors(CorsOptions options = CorsOptions{});

/**
 * @brief Rejects browser requests from origins that are not listed.
 *
 * Requests carrying an Origin header that is not in @p allowed_origins get
 * 403 Forbidden. Requests without an Origin header (non-browser clients) pass.
 * Use it on WebSocket routes to prevent cross-site WebSocket hijacking, since
 * browsers do not apply CORS to WebSocket handshakes:
 *
 * @code
 * app.ws("/chat", {middleware::require_origin({"https://app.example.com"})}, handler);
 * @endcode
 */
routing::Middleware require_origin(std::vector<std::string> allowed_origins);

} // namespace middleware
