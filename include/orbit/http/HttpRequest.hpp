#pragma once
#include <orbit/legacy_namespaces.hpp>
#ifdef _WIN32
#undef DELETE
#endif
#include <charconv>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <orbit/http/json.hpp>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/RequestContext.hpp>
#include <orbit/utils/CaseInsensitive.hpp>

namespace orbit::middleware {
class Session;
}

namespace orbit::http {

/**
 * @brief Represents standard HTTP methods.
 */
enum class HttpMethod { GET, POST, PUT, PATCH, DELETE, OPTIONS, HEAD, UNKNOWN };

/// Parses @p text as T (see HttpRequest::param); null if it does not parse
/// as a T with nothing left over.
template <typename T>
std::optional<T> parse_param(const std::string& text) {
    if constexpr (std::is_same_v<T, std::string>) {
        return text;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "true" || text == "1") return true;
        if (text == "false" || text == "0") return false;
        return std::nullopt;
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        T value{};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
        return value;
    } else {
        static_assert(!sizeof(T), "HttpRequest::param<T>() supports std::string, bool, and integral or "
                                  "floating-point T");
    }
}

/**
 * @brief Represents an incoming HTTP request.
 */
struct HttpRequest {
    HttpMethod method{HttpMethod::UNKNOWN};
    std::string uri;
    std::string target; // Raw request-target as received, including any query string
    std::unordered_map<std::string, std::string> query;
    std::string http_version;
    std::unordered_map<std::string_view, std::string_view, utils::CaseInsensitiveHash, utils::CaseInsensitiveEqual> headers;
    std::string_view body;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> cookies;
    std::string client_ip; ///< Client address; may be replaced by middleware::trusted_proxies()
    std::string peer_ip;   ///< Address of the socket peer (the client, or the last proxy)
    std::string session_id; // Set by the session middleware
    std::shared_ptr<middleware::Session> session; ///< Set by the session middleware
    std::string request_id; ///< Set by middleware::request_id()
    std::string trace_id;   ///< W3C trace id (32 hex), set by middleware::tracing()
    std::string span_id;    ///< This request's span id (16 hex), set by middleware::tracing()
    nlohmann::json user; // Populated by JwtAuth middleware

    /**
     * @brief Type-indexed storage for values middleware computes for later
     *        middleware or the handler (a tenant, a DB transaction, timing)
     *        with no fixed HttpRequest field or global needed. See
     *        RequestContext; `set`/`get`/`ensure` below forward to it.
     */
    RequestContext context;

    /// context.set<T>(value): see RequestContext::set.
    template <typename T>
    T& set(T value) {
        return context.set<T>(std::move(value));
    }
    /// context.get<T>(): see RequestContext::get.
    template <typename T>
    T* get() {
        return context.get<T>();
    }
    template <typename T>
    const T* get() const {
        return context.get<T>();
    }
    /// context.ensure<T>(args...): see RequestContext::ensure.
    template <typename T, typename... Args>
    T& ensure(Args&&... args) {
        return context.ensure<T>(std::forward<Args>(args)...);
    }

    mutable nlohmann::json json_body; // Cached parsed JSON

    /**
     * @brief Storage for header names and values that did not come from the
     *        raw request buffer. A deque never moves its elements, so the
     *        string_views in `headers` stay valid as it grows.
     */
    std::deque<std::string> owned_header_storage;

    /**
     * @brief Whatever owns this request's storage, when it is shared.
     *
     * Set by the HTTP/1.1 connection. A coroutine handler that takes the
     * request as a parameter holds it (see keep_alive()), so the request,
     * its headers and its body stay valid until the coroutine finishes,
     * even after the response is sent and the connection has moved on to
     * the next request. Empty when the request is owned another way (HTTP/2
     * and HTTP/3 streams are kept alive by their ResponseWriter).
     */
    std::weak_ptr<void> storage_owner;

    /// A reference that keeps this request's storage alive; null if
    /// storage_owner is empty or already gone.
    std::shared_ptr<void> keep_alive() const { return storage_owner.lock(); }

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
     * @brief A path parameter (from `params`; not `query`) parsed as `T`.
     * @return The value, or std::nullopt if @p name is absent or its text
     *         does not parse as `T` (trailing characters included).
     *
     * A route pattern that constrains the segment's type (`{id:int}`)
     * already guarantees the text parses as that type's full range, so
     * `param<std::int64_t>("id")` there is never null for a request the
     * route matched — `param<T>` for a narrower `T` (plain `int`, say)
     * still can be, for a value the route accepted but that overflows `T`.
     * `param` is just as useful for an untyped `:name` segment, where it
     * replaces `params.at(name)` plus a hand-written `std::stoi` (and the
     * try/catch an invalid one needs). It does not read `query`; copy a
     * value from there into `params` first if you want the same parsing
     * for it.
     *
     * @code
     * app.get("/tasks/:id", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
     *     auto id = req.param<int>("id");
     *     if (!id) { w->send(HttpResponse().status(HttpStatus::NotFound)); return; }
     *     // ...
     * });
     * @endcode
     */
    template <typename T>
    std::optional<T> param(const std::string& name) const {
        auto it = params.find(name);
        if (it == params.end()) return std::nullopt;
        return parse_param<T>(it->second);
    }

    /**
     * @brief Parses an application/x-www-form-urlencoded body (an HTML form
     *        submitted without files).
     * @return The decoded fields; empty if the Content-Type is not
     *         application/x-www-form-urlencoded or the body is malformed.
     */
    std::unordered_map<std::string, std::string> form_fields() const;

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
