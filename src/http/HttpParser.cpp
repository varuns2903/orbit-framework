#include <orbit/http/HttpParser.hpp>
#include <orbit/http/Http1Parser.hpp>
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

void parse_cookie_header(std::string_view cookie_str, std::unordered_map<std::string, std::string>& out) {
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
        out[std::string(key)] = std::string(val);
    }
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
    // One complete request, parsed by Http1Parser (llhttp) exactly as a
    // connection parses it. Bytes after it, such as a pipelined request, are
    // ignored; an incomplete or malformed request gives nullopt.
    Http1Parser parser;
    std::string_view rest = raw_request;
    for (int pass = 0; pass < 2; ++pass) { // the headers, then the body
        size_t used = 0;
        const Http1Parser::Event event = parser.feed(rest, used);
        rest.remove_prefix(used);
        if (event == Http1Parser::Event::MessageComplete) {
            HttpRequest request = std::move(parser.request());
            // The body lives in the parser, which is about to go. Keep it in
            // the request's own storage: a deque, so moving the request
            // leaves it (and the header views) where they are.
            const std::string& body = request.owned_header_storage.emplace_back(request.body);
            request.body = body;
            return request;
        }
        if (event != Http1Parser::Event::HeadersComplete) return std::nullopt;
    }
    return std::nullopt;
}

} // namespace http
