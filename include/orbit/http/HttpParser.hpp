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
 * @brief Utility class for parsing HTTP requests and related components.
 */
class HttpParser {
public:
    /**
     * @brief Parses a raw HTTP request string into a structured HttpRequest object.
     * @param raw_request The raw string view of the HTTP request.
     * @return std::optional<HttpRequest> containing the parsed request, or std::nullopt if the request is malformed.
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
