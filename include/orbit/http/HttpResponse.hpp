#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <orbit/http/json.hpp>
#include <orbit/utils/CaseInsensitive.hpp>

namespace http {

/**
 * @brief Standard HTTP status codes.
 */
enum class HttpStatus {
    Continue = 100,
    SwitchingProtocols = 101,
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    PartialContent = 206,
    MovedPermanently = 301,
    Found = 302,
    SeeOther = 303,
    NotModified = 304,
    TemporaryRedirect = 307,
    PermanentRedirect = 308,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    NotAcceptable = 406,
    RequestTimeout = 408,
    Conflict = 409,
    Gone = 410,
    LengthRequired = 411,
    PreconditionFailed = 412,
    PayloadTooLarge = 413,
    URITooLong = 414,
    UnsupportedMediaType = 415,
    RangeNotSatisfiable = 416,
    ExpectationFailed = 417,
    UnprocessableEntity = 422,
    UpgradeRequired = 426,
    TooManyRequests = 429,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    GatewayTimeout = 504
};

/**
 * @brief Reason phrase for a status code. Codes without a registered phrase
 *        get a generic one for their class (e.g. "Client Error" for 4xx).
 */
const char* reason_phrase(int status_code);

/**
 * @brief True if @p name is a valid header field name (an RFC 9110 token).
 */
bool is_valid_header_name(std::string_view name);

/**
 * @brief True if @p value can be written as a header field value: it
 *        contains no CR, LF or NUL, so it cannot start a new header line.
 */
bool is_valid_header_value(std::string_view value);

/**
 * @brief Represents an HTTP cookie.
 */
struct Cookie {
    std::string name;
    std::string value;
    std::string path = "/";
    std::string domain;
    long max_age = -1;
    bool secure = false;
    bool http_only = false;
    std::string same_site; // "Strict", "Lax", "None"
};

/**
 * @brief Represents an HTTP response to be sent to a client.
 */
class HttpResponse {
public:
    std::vector<Cookie> cookies;
    HttpStatus status_code = HttpStatus::OK;
    std::unordered_map<std::string, std::string, utils::CaseInsensitiveHash, utils::CaseInsensitiveEqual> headers;
    std::string body;
    
    // Zero-copy file descriptors. Bytes [file_offset, file_size) are sent.
    int file_fd{-1};
    off_t file_size{0};
    off_t file_offset{0};

    HttpResponse() = default;

    // Move semantics to manage the file_fd lifecycle safely
    HttpResponse(HttpResponse&& other) noexcept;
    HttpResponse& operator=(HttpResponse&& other) noexcept;
    ~HttpResponse();

    // Disable copy to prevent double-closing FDs
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;

    /**
     * @brief Adds a cookie to the response.
     * @param cookie The Cookie object to add.
     * @return A reference to this HttpResponse for method chaining.
     */
    HttpResponse& set_cookie(const Cookie& cookie) {
        cookies.push_back(cookie);
        return *this;
    }

    /**
     * @brief Adds a simple key-value cookie to the response.
     * @param name The name of the cookie.
     * @param value The value of the cookie.
     * @return A reference to this HttpResponse for method chaining.
     */
    HttpResponse& set_cookie(const std::string& name, const std::string& value) {
        Cookie c;
        c.name = name;
        c.value = value;
        cookies.push_back(c);
        return *this;
    }

    /**
     * @brief Sets the response body and its content type.
     * @param b The response body content.
     * @param content_type The MIME type of the body (default: "text/plain").
     */
    void set_body(const std::string& b, const std::string& content_type = "text/plain");
    
    // Ergonomic fluent helpers
    /**
     * @brief Sets the HTTP status code of the response.
     * @param code The HttpStatus code to set.
     * @return A reference to this HttpResponse for method chaining.
     */
    HttpResponse& status(HttpStatus code) & { status_code = code; return *this; }
    HttpResponse&& status(HttpStatus code) && { status_code = code; return std::move(*this); }

    /**
     * @brief Sets the response body as plain text.
     * @param b The text content to send.
     */
    HttpResponse& send(const std::string& b) & { set_body(b, "text/plain"); return *this; }
    HttpResponse&& send(const std::string& b) && { set_body(b, "text/plain"); return std::move(*this); }
    
    /**
     * @brief Sets the response body as JSON from a string.
     * @param j The JSON string content.
     */
    HttpResponse& json(const std::string& j) & { set_body(j, "application/json"); return *this; }
    HttpResponse&& json(const std::string& j) && { set_body(j, "application/json"); return std::move(*this); }
    
    /**
     * @brief Sets the response body as JSON from a nlohmann::json object.
     * @param j The nlohmann::json object to send.
     */
    HttpResponse& json(const nlohmann::json& j) & { set_body(j.dump(), "application/json"); return *this; }
    HttpResponse&& json(const nlohmann::json& j) && { set_body(j.dump(), "application/json"); return std::move(*this); }
    
    /**
     * @brief Sets the response body as HTML.
     * @param h The HTML string content.
     */
    HttpResponse& html(const std::string& h) & { set_body(h, "text/html"); return *this; }
    HttpResponse&& html(const std::string& h) && { set_body(h, "text/html"); return std::move(*this); }
    
    // Server-Side Rendering
    /**
     * @brief Renders a template and sets it as the HTML response body.
     * @param template_path The path to the template file.
     * @param data The JSON data to inject into the template.
     */
    void render(const std::string& template_path, const nlohmann::json& data);
    
    // Opens the file and sets up headers for sendfile()
    /**
     * @brief Prepares a file to be sent using zero-copy (sendfile).
     * @param path The path to the file to send.
     * @param content_type The MIME type of the file.
     */
    void send_file(const std::string& path, const std::string& content_type);

    /**
     * @brief Limits a send_file() response to bytes [start, end) of the file
     *        (clamped to its size) and updates Content-Length. The caller sets
     *        the 206 status and Content-Range.
     */
    void set_file_range(off_t start, off_t end);

    /**
     * @brief Serializes the entire response (headers and body) to a string.
     * @return The serialized HTTP response string.
     */
    std::string serialize() const;

    /**
     * @brief Serializes only the response headers to a string.
     * @return The serialized HTTP response headers string.
     */
    std::string serialize_headers() const;
};

} // namespace http
