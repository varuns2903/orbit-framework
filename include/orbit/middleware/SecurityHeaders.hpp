#pragma once
#include <orbit/routing/Router.hpp>
#include <chrono>
#include <string>

namespace middleware {

/**
 * @brief Options for security_headers(). An empty string leaves that header out.
 */
struct SecurityHeadersOptions {
    /// Strict-Transport-Security. Browsers ignore it on plain HTTP, so it is
    /// safe to send everywhere; only enable it once the site works over HTTPS.
    std::chrono::seconds hsts_max_age{std::chrono::hours(24 * 365)};
    bool hsts_include_subdomains = true;
    bool hsts_preload = false;
    bool hsts = true;

    std::string content_type_options = "nosniff";
    std::string frame_options = "DENY";                                  ///< X-Frame-Options
    std::string referrer_policy = "strict-origin-when-cross-origin";
    std::string cross_origin_opener_policy = "same-origin";
    /// Content-Security-Policy. Off by default because a policy must be written
    /// for the application; "frame-ancestors 'none'" alone is a safe start.
    std::string content_security_policy;
    std::string permissions_policy;                                      ///< e.g. "geolocation=(), camera=()"
};

/**
 * @ingroup middlewares
 * @brief Adds common security response headers (HSTS, nosniff, frame and
 *        referrer policies, COOP, optional CSP). They are defaults: a handler
 *        that sets the same header keeps its own value.
 */
routing::Middleware security_headers(SecurityHeadersOptions options = {});

} // namespace middleware
