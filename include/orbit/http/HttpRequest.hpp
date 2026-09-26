#pragma once
#ifdef _WIN32
#undef DELETE
#endif
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <orbit/http/json.hpp>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/utils/CaseInsensitive.hpp>

namespace http {

/**
 * @brief Represents standard HTTP methods.
 */
enum class HttpMethod { GET, POST, PUT, PATCH, DELETE, OPTIONS, HEAD, UNKNOWN };

/**
 * @brief Represents an incoming HTTP request.
 */
struct HttpRequest {
    HttpMethod method{HttpMethod::UNKNOWN};
    std::string uri;
    std::unordered_map<std::string, std::string> query;
    std::string http_version;
    std::unordered_map<std::string_view, std::string_view, utils::CaseInsensitiveHash, utils::CaseInsensitiveEqual> headers;
    std::string_view body;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> cookies;
    std::string client_ip;
    std::string session_id; // Set by SessionManager middleware
    nlohmann::json user; // Populated by JwtAuth middleware

    mutable nlohmann::json json_body; // Cached parsed JSON

    /**
     * @brief Storage for header names and values that did not come from the
     *        raw request buffer. A deque never moves its elements, so the
     *        string_views in `headers` stay valid as it grows.
     */
    std::deque<std::string> owned_header_storage;

    /**
     * @brief Sets a header, copying the name and value into storage owned by
     *        this request.
     *
     * `headers` holds string_views. Assigning a temporary or local string to
     * it directly leaves a dangling view once that string is destroyed; use
     * this instead whenever the value does not live in the request buffer.
     */
    void set_header(std::string_view name, std::string value) {
        const std::string& stored_name = owned_header_storage.emplace_back(name);
        const std::string& stored_value = owned_header_storage.emplace_back(std::move(value));
        auto it = headers.find(stored_name);
        if (it != headers.end()) {
            it->second = stored_value;
        } else {
            headers.emplace(stored_name, stored_value);
        }
    }
    
    /**
     * @brief Parses and returns the request body as a JSON object.
     * @details Caches the parsed JSON object for subsequent calls.
     * @return nlohmann::json object containing the parsed body, or an empty object if the body is empty or invalid.
     */
    nlohmann::json json() const {
        if (!json_body.empty()) return json_body;
        if (body.empty()) return nlohmann::json::object();
        json_body = nlohmann::json::parse(body, nullptr, false); // false = no exceptions
        return json_body;
    }

    /**
     * @brief Parses the request body as multipart/form-data.
     * @return MultipartForm object containing the parsed fields and files. Returns an empty form if the content type is not multipart/form-data.
     */
    MultipartForm form() const {
        auto ct = headers.find("Content-Type");
        if (ct != headers.end() && ct->second.starts_with("multipart/form-data")) {
            return MultipartForm::parse(ct->second, body);
        }
        return {};
    }
};

} // namespace http
