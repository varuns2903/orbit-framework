#pragma once
#include <orbit/routing/Router.hpp>
#include <orbit/utils/Logger.hpp>
#include <chrono>
#include <functional>
#include <string>

namespace middleware {

/**
 * @brief Options for request_id().
 */
struct RequestIdOptions {
    std::string header = "X-Request-ID";
    /// Reuse a well-formed ID sent by the client or an upstream proxy (up to
    /// 128 characters of [A-Za-z0-9._:-]); otherwise always generate one.
    bool trust_incoming = true;
};

/**
 * @ingroup middlewares
 * @brief Gives every request an ID (req.request_id) and echoes it in the
 *        response header, so a client report can be matched to server logs.
 */
routing::Middleware request_id(RequestIdOptions options = {});

/**
 * @brief A finished request, as reported to TracingOptions::on_span_end.
 */
struct Span {
    std::string trace_id;        ///< 32 hex digits
    std::string span_id;         ///< 16 hex digits
    std::string parent_span_id;  ///< From the incoming traceparent; empty for a root span
    std::string name;            ///< "GET /orders/42"
    std::chrono::system_clock::time_point start;
    std::chrono::microseconds duration{0};
    int status = 0;
    std::string client_ip;
};

/**
 * @brief Options for tracing().
 */
struct TracingOptions {
    /// Called once per request when its response is sent, e.g. to export
    /// the span to an OpenTelemetry collector. Runs on the thread that sent
    /// the response; keep it fast or hand the span to a queue.
    std::function<void(const Span&)> on_span_end;
};

/**
 * @ingroup middlewares
 * @brief W3C Trace Context: continues the trace from an incoming
 *        `traceparent` header (or starts a new one), sets req.trace_id and
 *        req.span_id, and reports each request as a Span.
 *
 * Pass `"00-" + req.trace_id + "-" + req.span_id + "-01"` as `traceparent`
 * on outgoing calls to link them to this request.
 */
routing::Middleware tracing(TracingOptions options = {});

/**
 * @brief Options for access_log().
 */
struct AccessLogOptions {
    utils::LogLevel level = utils::LogLevel::INFO;
};

/**
 * @ingroup middlewares
 * @brief Logs one line per request when its response is sent: method, path,
 *        status, response bytes, duration, client IP, user agent, and the
 *        request/trace IDs when request_id() / tracing() are used. In JSON
 *        log format these are separate fields.
 */
routing::Middleware access_log(AccessLogOptions options = {});

} // namespace middleware
