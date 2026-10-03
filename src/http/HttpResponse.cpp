#include <orbit/http/HttpResponse.hpp>
#ifdef _WIN32
#include <io.h>
#define open _open
#define close _close
#endif
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <inja/inja.hpp>
#include <orbit/utils/Logger.hpp>
#include <cctype>

namespace http {

HttpResponse::HttpResponse(HttpResponse&& other) noexcept 
    : status_code(other.status_code),
      headers(std::move(other.headers)),
      cookies(std::move(other.cookies)),
      body(std::move(other.body)),
      file_fd(other.file_fd),
      file_size(other.file_size) {
    other.file_fd = -1; // Steal ownership
}

HttpResponse& HttpResponse::operator=(HttpResponse&& other) noexcept {
    if (this != &other) {
        if (file_fd != -1) close(file_fd);
        status_code = other.status_code;
        headers = std::move(other.headers);
        cookies = std::move(other.cookies);
        body = std::move(other.body);
        file_fd = other.file_fd;
        file_size = other.file_size;
        other.file_fd = -1;
    }
    return *this;
}

HttpResponse::~HttpResponse() {
    if (file_fd != -1) {
        close(file_fd);
    }
}

void HttpResponse::set_body(const std::string& b, const std::string& content_type) {
    body = b;
    headers["Content-Type"] = content_type;
    headers["Content-Length"] = std::to_string(body.length());
}

void HttpResponse::render(const std::string& template_path, const nlohmann::json& data) {
    try {
        inja::Environment env;
        std::string result = env.render_file(template_path, data);
        set_body(result, "text/html");
    } catch (const std::exception& e) {
        status_code = HttpStatus::InternalServerError;
        set_body(std::string("<h1>500 Internal Server Error</h1><p>Template Error: ") + e.what() + "</p>", "text/html");
    }
}

void HttpResponse::send_file(const std::string& path, const std::string& content_type) {
    if (file_fd != -1) {
        close(file_fd);
    }
    
    file_fd = open(path.c_str(), O_RDONLY);
    if (file_fd == -1) {
        status_code = HttpStatus::NotFound;
        set_body("<h1>404 Not Found</h1>", "text/html");
        return;
    }
    
    struct stat stat_buf;
    if (fstat(file_fd, &stat_buf) == 0) {
        file_size = stat_buf.st_size;
        headers["Content-Length"] = std::to_string(file_size);
        headers["Content-Type"] = content_type;
    } else {
        close(file_fd);
        file_fd = -1;
        status_code = HttpStatus::InternalServerError;
        set_body("<h1>500 Internal Error</h1>", "text/html");
    }
}

const char* reason_phrase(int status_code) {
    switch (status_code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Entity";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 511: return "Network Authentication Required";
        default: break;
    }
    switch (status_code / 100) {
        case 1: return "Informational";
        case 2: return "Success";
        case 3: return "Redirection";
        case 4: return "Client Error";
        default: return "Server Error";
    }
}

bool is_valid_header_name(std::string_view name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        switch (c) {
            case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
            case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool is_valid_header_value(std::string_view value) {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') return false;
    }
    return true;
}

namespace {

// Cookie names are tokens; values and attributes must not contain the
// characters that end the cookie or the header line.
bool is_safe_cookie_part(std::string_view s) {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0' || c == ';') return false;
    }
    return true;
}

} // namespace

std::string HttpResponse::serialize_headers() const {
    // Not "oss": the LOG_* macros declare their own oss, and MSVC /WX
    // rejects the shadowing (C4456).
    std::ostringstream out;
    int code = static_cast<int>(status_code);
    if (code < 100 || code > 999) {
        LOG_ERROR("Invalid HTTP status code " << code << "; sending 500 instead");
        code = 500;
    }
    out << "HTTP/1.1 " << code << " " << reason_phrase(code) << "\r\n";

    bool has_content_length = false;
    for (const auto& [key, value] : headers) {
        // A CR or LF from application data would end this header and let the
        // rest of the value become new headers or a new response.
        if (!is_valid_header_name(key) || !is_valid_header_value(value)) {
            LOG_WARN("Dropping response header with invalid name or value: " << key);
            continue;
        }
        if (utils::CaseInsensitiveEqual{}(std::string_view(key), std::string_view("Content-Length"))) has_content_length = true;
        out << key << ": " << value << "\r\n";
    }

    for (const auto& cookie : cookies) {
        if (!is_valid_header_name(cookie.name) || !is_safe_cookie_part(cookie.value) ||
            !is_safe_cookie_part(cookie.path) || !is_safe_cookie_part(cookie.domain) ||
            !is_safe_cookie_part(cookie.same_site)) {
            LOG_WARN("Dropping cookie with invalid characters: " << cookie.name);
            continue;
        }
        out << "Set-Cookie: " << cookie.name << "=" << cookie.value;
        if (!cookie.path.empty()) out << "; Path=" << cookie.path;
        if (!cookie.domain.empty()) out << "; Domain=" << cookie.domain;
        if (cookie.max_age >= 0) out << "; Max-Age=" << cookie.max_age;
        if (cookie.secure) out << "; Secure";
        if (cookie.http_only) out << "; HttpOnly";
        if (!cookie.same_site.empty()) out << "; SameSite=" << cookie.same_site;
        out << "\r\n";
    }

    if (!has_content_length && !body.empty()) {
        out << "Content-Length: " << body.length() << "\r\n";
    }

    out << "\r\n";
    return out.str();
}

std::string HttpResponse::serialize() const {
    if (file_fd != -1) {
        return serialize_headers();
    }
    return serialize_headers() + body;
}

} // namespace http
