#include <orbit/middleware/OAuth2.hpp>
#include <orbit/utils/Logger.hpp>
#include <curl/curl.h>
#include <iostream>

namespace middleware {

namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    size_t total_size = size * nmemb;
    userp->append((char*)contents, total_size);
    return total_size;
}

} // namespace

OAuth2::OAuth2(const OAuth2Config& config) : config_(config) {}

std::string OAuth2::fetch_sync(const std::string& url, const std::string& method, const std::string& body, const std::string& auth_header) const {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;

    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "User-Agent: Orbit-Framework/1.0");
        headers = curl_slist_append(headers, "Accept: application/json");
        
        if (!auth_header.empty()) {
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
            }
        }
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        
        // Using thread pool for this in a real async environment, but it's okay for our current architecture
        // since we are just bridging it in a callback. Ideally this should be async using curl_multi.
        res = curl_easy_perform(curl);
        
        if(res != CURLE_OK) {
            LOG_ERROR("curl_easy_perform() failed: " << curl_easy_strerror(res));
        }
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return readBuffer;
}

std::function<void(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> OAuth2::login_handler() {
    return [this](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
        std::string scopes_str;
        for (size_t i = 0; i < config_.scopes.size(); ++i) {
            scopes_str += config_.scopes[i];
            if (i < config_.scopes.size() - 1) scopes_str += "%20";
        }

        std::string auth_url = config_.authorization_endpoint + 
            "?client_id=" + config_.client_id + 
            "&redirect_uri=" + config_.redirect_uri + 
            "&response_type=code" + 
            "&scope=" + scopes_str;

        http::HttpResponse res;
        res.status(http::HttpStatus::Found);
        res.headers["Location"] = auth_url;
        writer->send(std::move(res));
    };
}

std::function<void(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> OAuth2::callback_handler(
    std::function<void(const nlohmann::json& user_info, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_success,
    std::function<void(const std::string& error, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> on_error
) {
    return [this, on_success, on_error](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
        // Extract authorization code from query params
        auto code_it = req.query.find("code");
        if (code_it == req.query.end()) {
            on_error("Authorization code missing", req, writer);
            return;
        }
        std::string code = code_it->second;

        // Exchange code for token
        std::string token_body = "client_id=" + config_.client_id + 
                                "&client_secret=" + config_.client_secret + 
                                "&code=" + code + 
                                "&redirect_uri=" + config_.redirect_uri + 
                                "&grant_type=authorization_code";
                                
        // We really should dispatch this to a thread pool because libcurl blocking is bad for the event loop.
        // Let's do it inline for now, but a production app must use thread_pool()!
        std::string token_response = fetch_sync(config_.token_endpoint, "POST", token_body);
        
        try {
            auto token_json = nlohmann::json::parse(token_response);
            if (!token_json.contains("access_token")) {
                on_error("Access token missing in response: " + token_response, req, writer);
                return;
            }
            
            std::string access_token = token_json["access_token"];
            std::string auth_header = "Authorization: Bearer " + access_token;
            
            // Fetch user info
            std::string user_info_response = fetch_sync(config_.userinfo_endpoint, "GET", "", auth_header);
            auto user_info = nlohmann::json::parse(user_info_response);
            
            on_success(user_info, req, writer);
        } catch (const std::exception& e) {
            on_error(std::string("JSON parse error: ") + e.what(), req, writer);
        }
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
