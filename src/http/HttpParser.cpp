#include <orbit/http/HttpParser.hpp>
#include <sstream>

#include <cctype>

namespace http {

bool connection_option_present(std::string_view field_value, std::string_view option) {
    size_t pos = 0;
    while (pos <= field_value.size()) {
        size_t comma = field_value.find(',', pos);
        std::string_view token = field_value.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);

        // Trim optional whitespace around the token.
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }

        if (token.size() == option.size()) {
            bool equal = true;
            for (size_t i = 0; i < token.size(); ++i) {
                if (static_cast<char>(std::tolower(static_cast<unsigned char>(token[i]))) != option[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }

        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return false;
}

HttpMethod HttpParser::parse_method(std::string_view method_str) {
    if (method_str == "GET") return HttpMethod::GET;
    if (method_str == "POST") return HttpMethod::POST;
    if (method_str == "PUT") return HttpMethod::PUT;
    if (method_str == "PATCH") return HttpMethod::PATCH;
    if (method_str == "DELETE") return HttpMethod::DELETE;
    if (method_str == "OPTIONS") return HttpMethod::OPTIONS;
    if (method_str == "HEAD") return HttpMethod::HEAD;
    return HttpMethod::UNKNOWN;
}

std::optional<HttpRequest> HttpParser::parse(std::string_view raw_request) {
    HttpRequest request;
    
    // Find the end of the request line (first CRLF)
    size_t request_line_end = raw_request.find("\r\n");
    if (request_line_end == std::string_view::npos) {
        return std::nullopt; // Malformed: No CRLF found
    }
    
    std::string_view request_line = raw_request.substr(0, request_line_end);
    
    // Parse Request Line: METHOD URI VERSION
    size_t space1 = request_line.find(' ');
    size_t space2 = request_line.find(' ', space1 + 1);
    
    if (space1 != std::string_view::npos && space2 != std::string_view::npos && space1 != space2) {
        request.method = parse_method(request_line.substr(0, space1));
        std::string full_uri = std::string(request_line.substr(space1 + 1, space2 - space1 - 1));
        
        auto q_mark = full_uri.find('?');
        if (q_mark != std::string::npos) {
            request.uri = full_uri.substr(0, q_mark);
            std::string query_string = full_uri.substr(q_mark + 1);
            
            std::istringstream q_stream(query_string);
            std::string kv;
            while (std::getline(q_stream, kv, '&')) {
                auto eq_pos = kv.find('=');
                if (eq_pos != std::string::npos) {
                    request.query[kv.substr(0, eq_pos)] = kv.substr(eq_pos + 1);
                } else {
                    request.query[kv] = ""; // Key with no value
                }
            }
        } else {
            request.uri = full_uri;
        }

        request.http_version = std::string(request_line.substr(space2 + 1));
    } else {
        return std::nullopt;
    }
    
    // Parse Headers
    size_t headers_start = request_line_end + 2;
    while (headers_start < raw_request.length()) {
        size_t line_end = raw_request.find("\r\n", headers_start);
        if (line_end == std::string_view::npos) return std::nullopt;
        
        // Empty line indicates end of headers
        if (line_end == headers_start) {
            headers_start += 2;
            break;
        }
        
        std::string_view header_line = raw_request.substr(headers_start, line_end - headers_start);
        size_t colon_pos = header_line.find(':');
        if (colon_pos != std::string_view::npos) {
            std::string_view key = header_line.substr(0, colon_pos);
            // Skip the colon and any following space
            size_t value_start = colon_pos + 1;
            while (value_start < header_line.length() && header_line[value_start] == ' ') {
                value_start++;
            }
            std::string_view value = header_line.substr(value_start);
            request.headers[key] = value;
        }
        
        headers_start = line_end + 2;
    }
    
    // Body is whatever is left
    if (headers_start < raw_request.length()) {
        request.body = raw_request.substr(headers_start);
    }
    
    // Parse cookies
    auto cookie_it = request.headers.find("Cookie");
    if (cookie_it != request.headers.end()) {
        std::string_view cookie_str = cookie_it->second;
        size_t pos = 0;
        while (pos < cookie_str.length()) {
            // Skip leading spaces
            while (pos < cookie_str.length() && cookie_str[pos] == ' ') pos++;
            
            size_t eq_pos = cookie_str.find('=', pos);
            if (eq_pos == std::string_view::npos) break; // Malformed cookie
            
            size_t semi_pos = cookie_str.find(';', eq_pos);
            std::string_view key = cookie_str.substr(pos, eq_pos - pos);
            std::string_view val;
            
            if (semi_pos != std::string_view::npos) {
                val = cookie_str.substr(eq_pos + 1, semi_pos - eq_pos - 1);
                pos = semi_pos + 1;
            } else {
                val = cookie_str.substr(eq_pos + 1);
                pos = cookie_str.length();
            }
            request.cookies[std::string(key)] = std::string(val);
        }
    }
    
    return request;
}

} // namespace http
