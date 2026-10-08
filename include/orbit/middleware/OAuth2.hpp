#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/http/json.hpp>

namespace orbit::middleware {

/**
 * @brief Provider endpoints, client credentials and flow settings for OAuth2.
 */
struct OAuth2Config {
    std::string client_id;
    std::string client_secret;
    std::string redirect_uri;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::string userinfo_endpoint;
    std::vector<std::string> scopes;

    bool use_pkce = true;             ///< Send an S256 PKCE challenge (RFC 7636)
    bool secure_cookies = false;      ///< Mark the state/PKCE cookies Secure (enable behind HTTPS)
    long connect_timeout_seconds = 5; ///< Provider connection timeout
    long request_timeout_seconds = 10;///< Total time allowed per provider request
};

/**
 * @brief Authorization-code flow helper.
 *
 * login_handler() redirects to the provider with a random `state` (and a PKCE
 * challenge), remembered in short-lived HttpOnly cookies. callback_handler()
 * rejects callbacks whose `state` does not match, which prevents login CSRF,
 * then exchanges the code (with the PKCE verifier) and fetches the user info.
 *
 * The handlers copy the configuration, so the OAuth2 object may be a temporary.
 * Provider requests block the calling worker thread for at most
 * request_timeout_seconds.
 */
class OAuth2 {
public:
    using Handler = std::function<void(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)>;

    OAuth2(const OAuth2Config& config);

    Handler login_handler() const;

    Handler callback_handler(
        std::function<void(const nlohmann::json& user_info, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_success,
        std::function<void(const std::string& error, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_error
    ) const;

    static OAuth2 google(const std::string& client_id, const std::string& client_secret, const std::string& redirect_uri);

    static OAuth2 github(const std::string& client_id, const std::string& client_secret, const std::string& redirect_uri);

    const OAuth2Config& config() const { return config_; }

private:
    OAuth2Config config_;
};

namespace detail {
/// application/x-www-form-urlencoded / URI component encoding (RFC 3986 unreserved kept).
std::string url_encode(const std::string& value);
/// Decodes %XX escapes and '+' as space.
std::string url_decode(const std::string& value);
/// RFC 7636 S256: base64url(SHA-256(verifier)) without padding.
std::string pkce_challenge(const std::string& verifier);
} // namespace detail

} // namespace middleware
