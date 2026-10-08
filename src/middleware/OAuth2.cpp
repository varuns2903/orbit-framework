#include <orbit/middleware/OAuth2.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/Client.hpp>
#include <orbit/utils/Logger.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cctype>
#include <mutex>
#include <stdexcept>

namespace orbit::middleware {

namespace {

constexpr const char* kStateCookie = "oauth_state";
constexpr const char* kVerifierCookie = "oauth_pkce";
constexpr long kFlowCookieSeconds = 600;

std::string base64url(const unsigned char* data, size_t len) {
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(len));
    out.resize(static_cast<size_t>(n));
    while (!out.empty() && out.back() == '=') out.pop_back();
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return out;
}

std::string random_token() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return base64url(bytes, sizeof(bytes));
}

struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
};

HttpResult fetch(const OAuth2Config& config, const std::string& url, const std::string& post_body,
                 const std::string& auth_header) {
    http::ClientRequest request;
    request.url = url;
    request.connect_timeout = std::chrono::seconds(config.connect_timeout_seconds);
    request.timeout = std::chrono::seconds(config.request_timeout_seconds);
    // A redirect would carry the client secret (and the code) elsewhere.
    request.follow_redirects = false;
    request.headers.push_back({"Accept", "application/json"});
    if (!auth_header.empty()) {
        size_t colon = auth_header.find(':');
        request.headers.push_back({auth_header.substr(0, colon), auth_header.substr(colon + 2)});
    }
    if (!post_body.empty()) {
        request.method = "POST";
        request.body = post_body;
        request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    }

    // Worker thread: blocking here is what the handler did before; the shared
    // client keeps connections to the provider alive between logins.
    http::ClientResponse response = http::Client::shared().send_sync(std::move(request));
    HttpResult result;
    if (!response.ok()) {
        result.error = response.error;
        LOG_ERROR("OAuth2 request to " << url << " failed: " << result.error);
        return result;
    }
    result.status = response.status;
    result.body = std::move(response.body);
    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok) result.error = "HTTP " + std::to_string(result.status);
    return result;
}

http::Cookie flow_cookie(const OAuth2Config& config, const std::string& name, const std::string& value, long max_age) {
    http::Cookie c;
    c.name = name;
    c.value = value;
    c.path = "/";
    c.http_only = true;
    c.secure = config.secure_cookies;
    c.same_site = "Lax"; // must survive the top-level redirect back from the provider
    c.max_age = max_age;
    return c;
}

} // namespace

namespace detail {

std::string url_encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string url_decode(const std::string& value) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size() && hexval(value[i + 1]) >= 0 && hexval(value[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexval(value[i + 1]) * 16 + hexval(value[i + 2])));
            i += 2;
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string pkce_challenge(const std::string& verifier) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), digest);
    return base64url(digest, sizeof(digest));
}

} // namespace detail

OAuth2::OAuth2(const OAuth2Config& config) : config_(config) {}

OAuth2::Handler OAuth2::login_handler() const {
    // Capture a copy: OAuth2::google(...).login_handler() is a common pattern
    // and the temporary is gone by the time a request arrives.
    return [config = config_](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
        std::string scopes_str;
        for (size_t i = 0; i < config.scopes.size(); ++i) {
            if (i > 0) scopes_str += " ";
            scopes_str += config.scopes[i];
        }

        std::string state = random_token();
        std::string auth_url = config.authorization_endpoint +
            (config.authorization_endpoint.find('?') == std::string::npos ? "?" : "&") +
            "client_id=" + detail::url_encode(config.client_id) +
            "&redirect_uri=" + detail::url_encode(config.redirect_uri) +
            "&response_type=code" +
            "&scope=" + detail::url_encode(scopes_str) +
            "&state=" + detail::url_encode(state);

        http::HttpResponse res;
        res.status(http::HttpStatus::Found);
        res.set_cookie(flow_cookie(config, kStateCookie, state, kFlowCookieSeconds));

        if (config.use_pkce) {
            std::string verifier = random_token();
            auth_url += "&code_challenge=" + detail::pkce_challenge(verifier) + "&code_challenge_method=S256";
            res.set_cookie(flow_cookie(config, kVerifierCookie, verifier, kFlowCookieSeconds));
        }

        res.headers["Location"] = auth_url;
        writer->send(std::move(res));
    };
}

OAuth2::Handler OAuth2::callback_handler(
    std::function<void(const nlohmann::json& user_info, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_success,
    std::function<void(const std::string& error, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_error
) const {
    return [config = config_, on_success, on_error](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
        // One-shot values: clear them whatever the outcome.
        writer->add_interceptor([config](http::HttpResponse& res) {
            res.set_cookie(flow_cookie(config, kStateCookie, "", 0));
            if (config.use_pkce) res.set_cookie(flow_cookie(config, kVerifierCookie, "", 0));
        });

        auto provider_error = req.query.find("error");
        if (provider_error != req.query.end()) {
            on_error("Authorization failed: " + detail::url_decode(provider_error->second), req, writer);
            return;
        }

        // The state must round-trip through the provider unchanged; otherwise
        // this callback was not started by this browser (login CSRF).
        auto state_it = req.query.find("state");
        auto cookie_it = req.cookies.find(kStateCookie);
        std::string state = state_it == req.query.end() ? "" : detail::url_decode(state_it->second);
        std::string expected = cookie_it == req.cookies.end() ? "" : cookie_it->second;
        if (state.empty() || expected.empty() || state.size() != expected.size() ||
            CRYPTO_memcmp(state.data(), expected.data(), state.size()) != 0) {
            on_error("Invalid OAuth2 state", req, writer);
            return;
        }

        auto code_it = req.query.find("code");
        if (code_it == req.query.end()) {
            on_error("Authorization code missing", req, writer);
            return;
        }
        std::string code = detail::url_decode(code_it->second);

        std::string token_body = "grant_type=authorization_code"
            "&code=" + detail::url_encode(code) +
            "&redirect_uri=" + detail::url_encode(config.redirect_uri) +
            "&client_id=" + detail::url_encode(config.client_id) +
            "&client_secret=" + detail::url_encode(config.client_secret);
        if (config.use_pkce) {
            auto verifier_it = req.cookies.find(kVerifierCookie);
            if (verifier_it == req.cookies.end() || verifier_it->second.empty()) {
                on_error("PKCE verifier missing", req, writer);
                return;
            }
            token_body += "&code_verifier=" + detail::url_encode(verifier_it->second);
        }

        HttpResult token_response = fetch(config, config.token_endpoint, token_body, "");
        if (!token_response.ok) {
            on_error("Token request failed: " + token_response.error, req, writer);
            return;
        }

        nlohmann::json token_json = nlohmann::json::parse(token_response.body, nullptr, false);
        if (token_json.is_discarded() || !token_json.is_object() ||
            !token_json.contains("access_token") || !token_json["access_token"].is_string()) {
            // Do not echo the response: it may contain credentials.
            on_error("Token response did not contain an access token", req, writer);
            return;
        }

        std::string auth_header = "Authorization: Bearer " + token_json["access_token"].get<std::string>();
        HttpResult user_response = fetch(config, config.userinfo_endpoint, "", auth_header);
        if (!user_response.ok) {
            on_error("User info request failed: " + user_response.error, req, writer);
            return;
        }
        nlohmann::json user_info = nlohmann::json::parse(user_response.body, nullptr, false);
        if (user_info.is_discarded()) {
            on_error("User info response is not JSON", req, writer);
            return;
        }

        on_success(user_info, req, writer);
    };
}

OAuth2 OAuth2::google(const std::string& client_id, const std::string& client_secret, const std::string& redirect_uri) {
    OAuth2Config config;
    config.client_id = client_id;
    config.client_secret = client_secret;
    config.redirect_uri = redirect_uri;
    config.authorization_endpoint = "https://accounts.google.com/o/oauth2/v2/auth";
    config.token_endpoint = "https://oauth2.googleapis.com/token";
    config.userinfo_endpoint = "https://www.googleapis.com/oauth2/v3/userinfo";
    config.scopes = {"openid", "profile", "email"};
    return OAuth2(config);
}

OAuth2 OAuth2::github(const std::string& client_id, const std::string& client_secret, const std::string& redirect_uri) {
    OAuth2Config config;
    config.client_id = client_id;
    config.client_secret = client_secret;
    config.redirect_uri = redirect_uri;
    config.authorization_endpoint = "https://github.com/login/oauth/authorize";
    config.token_endpoint = "https://github.com/login/oauth/access_token";
    config.userinfo_endpoint = "https://api.github.com/user";
    config.scopes = {"read:user", "user:email"};
    return OAuth2(config);
}

} // namespace middleware
