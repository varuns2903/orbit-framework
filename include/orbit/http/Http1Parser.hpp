#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/http/HttpRequest.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace orbit::http {

/**
 * @brief Incremental HTTP/1.1 request parser, built on llhttp (the parser
 *        Node.js uses).
 *
 * Bytes are fed as they arrive, in pieces of any size; nothing is rescanned.
 * Parsing is strict (RFC 9112, no lenient flags), so ambiguous framing that
 * could smuggle a request is rejected. The resulting HttpRequest owns its
 * header names, values and body.
 *
 * The parser pauses up to twice per request:
 *  - HeadersComplete, only when a body follows: method, target, headers and
 *    cookies are known; the body has not been read. The caller may install a
 *    body handler (to stream it) or send "100 Continue" first, then feeds the
 *    rest.
 *  - MessageComplete: the request is whole. The caller handles it, then calls
 *    next() before feeding the bytes that follow (a pipelined request).
 *
 * Limits are enforced as bytes arrive: an over-long request line or header
 * section fails before its terminating CRLF is seen, and a Content-Length
 * above the body limit fails before any of the body is read.
 */
class Http1Parser {
public:
    struct Limits {
        size_t max_request_line = 4096;
        size_t max_header_bytes = 8192; ///< Request line plus all header lines
        size_t max_headers = 100;
        size_t max_body_size = 10 * 1024 * 1024;
    };

    enum class Event {
        NeedMore,         ///< All input consumed; feed more.
        HeadersComplete,  ///< Paused after the header section of a request with a body.
        MessageComplete,  ///< Paused after a whole request; call next().
        Error,            ///< See error_status() and error_reason().
    };

    explicit Http1Parser(Limits limits);
    Http1Parser() : Http1Parser(Limits{}) {}
    ~Http1Parser();
    Http1Parser(const Http1Parser&) = delete;
    Http1Parser& operator=(const Http1Parser&) = delete;

    /// Parses from `data`, stopping at the next pause. `consumed` is set to
    /// the number of bytes used; the rest must be fed again later.
    Event feed(std::string_view data, size_t& consumed);

    /// After MessageComplete: discards the request and readies the parser
    /// for the next one on the same connection.
    void next();

    /// The request being parsed. Valid from HeadersComplete (or, without a
    /// body, MessageComplete); its body is set at MessageComplete (empty when
    /// a body handler took the body).
    HttpRequest& request();

    /// Receives the body as it arrives instead of buffering it. Set after
    /// HeadersComplete; cleared by next().
    void set_body_handler(std::function<void(std::string_view)> handler);

    /// Chooses the body size limit per request, from its method, path and
    /// headers, in place of Limits::max_body_size (e.g. none for a route that
    /// streams large uploads). Called once the headers are parsed, before a
    /// declared Content-Length is checked. Stays set across requests.
    void set_body_limit(std::function<size_t(const HttpRequest&)> limit_for);

    bool expect_continue() const; ///< "Expect: 100-continue" was sent.
    bool chunked() const;         ///< Transfer-Encoding: chunked.
    bool has_body() const;        ///< A body follows the headers (chunked or Content-Length > 0).
    bool keep_alive() const;      ///< The connection may carry another request.
    bool upgrade() const;         ///< At MessageComplete: the request asks to switch protocols.

    int error_status() const;            ///< 400, 413, 431 or 501 after Error.
    const std::string& error_reason() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace http
