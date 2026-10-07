#pragma once
#include <orbit/http/HttpRequest.hpp>
#include <optional>
#include <string_view>

namespace http {

/**
 * @brief Tests whether a Connection header field carries a given option.
 *
 * The Connection header is a comma-separated list of case-insensitive tokens
 * (RFC 9110 section 7.6.1), so a client may legitimately send
 * `Connection: keep-alive, TE` or `Connection: Close`. Comparing the whole
 * field value against a single token misses both.
 *
 * @param field_value The raw Connection header value.
 * @param option The option to look for, in lowercase (e.g. "close").
 * @return True if the option appears as one of the listed tokens.
 */
bool connection_option_present(std::string_view field_value, std::string_view option);

/**
 * @brief Decodes %XX escapes (RFC 3986 section 2.1).
 *
 * @param in The encoded text.
 * @param out Receives the decoded text.
 * @param plus_as_space Decode '+' as a space (query strings / form data).
 * @param for_path Reject escapes that would change how a path is split or
 *        read: %2F ('/'), %5C ('\\') and %00. A request that hides a path
 *        separator in an escape is refused rather than guessed at.
 * @return False on a malformed escape or a rejected character.
 */
bool percent_decode(std::string_view in, std::string& out, bool plus_as_space, bool for_path);

/**
 * @brief Decodes application/x-www-form-urlencoded data: a query string or
 *        a form body (WHATWG URL Standard section 5.1).
 *
 * Pairs are separated by '&', names from values by the first '='; '+' is a
 * space. A name without '=' gets an empty value, and a repeated name keeps
 * its last value.
 *
 * @return False on a malformed escape; @p out may then hold earlier pairs.
 */
bool parse_urlencoded(std::string_view data, std::unordered_map<std::string, std::string>& out);

/// Adds the name=value pairs of a Cookie header to `out` (a later pair with
/// the same name wins).
void parse_cookie_header(std::string_view cookie_header, std::unordered_map<std::string, std::string>& out);

/**
 * @brief Utility class for parsing HTTP requests and related components.
 */
class HttpParser {
public:
    /**
     * @brief Parses one complete HTTP/1.1 request held in memory.
     *
     * Uses Http1Parser (llhttp), with the same strict rules and default
     * limits as a connection. The request owns its headers and body, so it
     * outlives @p raw_request. Anything after the first request is ignored.
     *
     * @param raw_request The raw request, including any body.
     * @return The request, or std::nullopt if it is malformed or incomplete.
     */
    static std::optional<HttpRequest> parse(std::string_view raw_request);

    /**
     * @brief Parses an HTTP method string into an HttpMethod enum value.
     * @param method_str The string representation of the HTTP method (e.g., "GET").
     * @return The corresponding HttpMethod enum value, or HttpMethod::UNKNOWN if not recognized.
     */
    static HttpMethod parse_method(std::string_view method_str);
};

} // namespace http
