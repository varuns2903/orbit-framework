#include <orbit/http/HttpParser.hpp>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

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

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Parses a non-empty run of decimal digits, rejecting signs, spaces and overflow.
bool parse_decimal(std::string_view s, size_t& out) {
    if (s.empty()) return false;
    size_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        size_t digit = static_cast<size_t>(c - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool is_token_char(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

} // namespace

MessageFraming parse_framing(std::string_view header_section) {
    MessageFraming framing;
    bool has_transfer_encoding = false;

    size_t pos = 0;
    while (pos < header_section.size()) {
        size_t line_end = header_section.find("\r\n", pos);
        if (line_end == std::string_view::npos) line_end = header_section.size();
        std::string_view line = header_section.substr(pos, line_end - pos);
        pos = line_end + 2;
        if (line.empty()) continue;

        size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            framing.valid = false;
            return framing;
        }
        std::string_view name = line.substr(0, colon);
        for (char c : name) {
            if (!is_token_char(c)) {
                // Covers "Content-Length : 5" and obs-fold continuation lines.
                framing.valid = false;
                return framing;
            }
        }
        std::string_view value = trim_ows(line.substr(colon + 1));

        if (iequals(name, "Content-Length")) {
            size_t length = 0;
            if (!parse_decimal(value, length)) {
                framing.valid = false;
                return framing;
            }
            if (framing.has_content_length && framing.content_length != length) {
                framing.valid = false;
                return framing;
            }
            framing.has_content_length = true;
            framing.content_length = length;
        } else if (iequals(name, "Transfer-Encoding")) {
            has_transfer_encoding = true;
            // The final coding decides framing; only "chunked" is understood.
            size_t last_comma = value.rfind(',');
            std::string_view last = trim_ows(last_comma == std::string_view::npos ? value : value.substr(last_comma + 1));
            framing.chunked = iequals(last, "chunked");
        } else if (iequals(name, "Expect")) {
            framing.expect_continue = iequals(value, "100-continue");
        }
    }

    if (has_transfer_encoding && (!framing.chunked || framing.has_content_length)) {
        framing.valid = false;
    }
    return framing;
}

ChunkedStatus decode_chunked(std::string_view data, std::string& out, size_t& consumed, size_t max_size) {
    out.clear();
    size_t pos = 0;
    while (true) {
        size_t line_end = data.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            // A chunk-size line longer than this is not something we accept.
            return data.size() - pos > 1024 ? ChunkedStatus::Invalid : ChunkedStatus::Incomplete;
        }

        std::string_view size_line = data.substr(pos, line_end - pos);
        size_t ext = size_line.find(';');
        std::string_view hex = trim_ows(ext == std::string_view::npos ? size_line : size_line.substr(0, ext));
        if (hex.empty() || hex.size() > 16) return ChunkedStatus::Invalid;

        size_t chunk_size = 0;
        for (char c : hex) {
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return ChunkedStatus::Invalid;
            if (chunk_size > (SIZE_MAX >> 4)) return ChunkedStatus::Invalid;
            chunk_size = (chunk_size << 4) | static_cast<size_t>(digit);
        }
        pos = line_end + 2;

        if (chunk_size == 0) {
            // Trailer section: zero or more field lines, then an empty line.
            while (true) {
                size_t trailer_end = data.find("\r\n", pos);
                if (trailer_end == std::string_view::npos) return ChunkedStatus::Incomplete;
                bool empty_line = (trailer_end == pos);
                pos = trailer_end + 2;
                if (empty_line) {
                    consumed = pos;
                    return ChunkedStatus::Complete;
                }
            }
        }

        if (chunk_size > max_size || out.size() > max_size - chunk_size) return ChunkedStatus::TooLarge;
        if (data.size() - pos < chunk_size + 2) return ChunkedStatus::Incomplete;
        if (data.substr(pos + chunk_size, 2) != "\r\n") return ChunkedStatus::Invalid;
        out.append(data.substr(pos, chunk_size));
        pos += chunk_size + 2;
    }
}

bool parse_urlencoded(std::string_view data, std::unordered_map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t amp = data.find('&', pos);
        std::string_view pair = data.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            std::string name, value;
            if (!percent_decode(pair.substr(0, eq), name, true, false) ||
                !percent_decode(eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1), value, true, false)) {
                return false;
            }
            out[std::move(name)] = std::move(value);
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return true;
}

std::unordered_map<std::string, std::string> HttpRequest::form_fields() const {
    std::unordered_map<std::string, std::string> fields;
    auto ct = headers.find("Content-Type");
    if (ct == headers.end()) return fields;
    // The media type, without parameters such as charset, is case-insensitive.
    std::string_view media = ct->second.substr(0, ct->second.find(';'));
    while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) media.remove_suffix(1);
    while (!media.empty() && (media.front() == ' ' || media.front() == '\t')) media.remove_prefix(1);
    constexpr std::string_view kForm = "application/x-www-form-urlencoded";
    if (media.size() != kForm.size() ||
        !std::equal(media.begin(), media.end(), kForm.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        })) {
        return fields;
    }
    if (!parse_urlencoded(body, fields)) fields.clear(); // all or nothing
    return fields;
}

bool percent_decode(std::string_view in, std::string& out, bool plus_as_space, bool for_path) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = hexval(in[i + 1]);
            int lo = hexval(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char decoded = static_cast<char>(hi * 16 + lo);
            if (for_path && (decoded == '/' || decoded == '\\' || decoded == '\0')) return false;
            out.push_back(decoded);
            i += 2;
        } else if (c == '+' && plus_as_space) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return true;
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
        request.target = full_uri;
        
        auto q_mark = full_uri.find('?');
        std::string_view raw_path = std::string_view(full_uri).substr(0, q_mark);
        // Routes, params and static files all see the decoded path.
        if (!percent_decode(raw_path, request.uri, false, true)) {
            return std::nullopt;
        }
        if (q_mark != std::string::npos &&
            !parse_urlencoded(std::string_view(full_uri).substr(q_mark + 1), request.query)) {
            return std::nullopt;
        }


        request.http_version = std::string(request_line.substr(space2 + 1));
    } else {
        return std::nullopt;
    }
    
    // Reject ambiguous or malformed framing before looking at anything else.
    size_t headers_end = raw_request.find("\r\n\r\n", request_line_end);
    std::string_view header_section = headers_end == std::string_view::npos
        ? std::string_view{}
        : raw_request.substr(request_line_end + 2, headers_end + 2 - (request_line_end + 2));
    MessageFraming framing = parse_framing(header_section);
    if (!framing.valid) {
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
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            request.headers[key] = value;
        }
        
        headers_start = line_end + 2;
    }
    
    // The body is exactly Content-Length bytes; anything after it belongs to
    // the next pipelined request. A chunked body is left encoded here and
    // decoded by the connection once it is complete.
    if (headers_start < raw_request.length()) {
        std::string_view rest = raw_request.substr(headers_start);
        if (framing.chunked) {
            request.body = rest;
        } else if (framing.has_content_length) {
            request.body = rest.substr(0, framing.content_length);
        }
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
