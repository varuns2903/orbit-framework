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
 * @brief How the length of a request body is determined (RFC 9112 section 6).
 */
struct MessageFraming {
    bool valid{true};                 ///< False if the framing headers are malformed or ambiguous.
    bool chunked{false};              ///< Transfer-Encoding ends in "chunked".
    bool has_content_length{false};   ///< A Content-Length header is present.
    size_t content_length{0};         ///< Its value, when has_content_length is true.
    bool expect_continue{false};      ///< "Expect: 100-continue" (RFC 9110 section 10.1.1).
};

/**
 * @brief Determines body framing from a request's header section.
 *
 * The header section is everything between the request line's CRLF and the
 * blank line that ends the headers. The request is rejected (valid == false)
 * when a header line has no colon or whitespace before the colon, when
 * Content-Length is not a plain decimal number or appears more than once with
 * different values, when Transfer-Encoding does not end in "chunked", or when
 * both Transfer-Encoding and Content-Length are present. Accepting any of
 * these lets a front-end proxy and this server disagree about where a request
 * ends.
 *
 * @param header_section The raw header lines, each terminated by CRLF.
 * @return The framing, with valid == false if the request must be rejected.
 */
MessageFraming parse_framing(std::string_view header_section);

/**
 * @brief Result of decoding a chunked request body.
 */
enum class ChunkedStatus { Incomplete, Complete, Invalid, TooLarge };

/**
 * @brief Decodes a chunked transfer-coded body (RFC 9112 section 7.1).
 *
 * @param data Bytes following the request's header section.
 * @param out Receives the decoded body when the result is Complete.
 * @param consumed Receives the number of bytes of @p data the encoded body
 *        occupies, including the last chunk and trailer section.
 * @param max_size Largest decoded body accepted.
 */
ChunkedStatus decode_chunked(std::string_view data, std::string& out, size_t& consumed, size_t max_size);

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
