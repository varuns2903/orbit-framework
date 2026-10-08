# Observability

Logs, request IDs, tracing and metrics for running Orbit in production.

```cpp
#include <orbit/middleware/Observability.hpp>
#include <orbit/middleware/Metrics.hpp>

orbit::config::ServerConfig cfg;
cfg.log_format = "json";                       // or --log-format json
orbit::server::App app(cfg);

app.use(orbit::middleware::request_id());             // X-Request-ID in and out
app.use(orbit::middleware::tracing());                // W3C traceparent
app.use(orbit::middleware::access_log());             // one line per request
app.use(orbit::middleware::Metrics::track());         // Prometheus metrics
app.enable_metrics("/metrics");
```

Register `request_id()` and `tracing()` before `access_log()`, so the log line
carries their IDs.

## Logs

`ServerConfig::log_format` (`--log-format`) selects the format:

- `text` (default): `[2026-10-04 12:00:00.123] [INFO ] [App.cpp:296] message key=value`.
  Colors are used only when stdout is a terminal.
- `json`: one object per line, timestamps in UTC, ready for Loki, ELK, Datadog,
  CloudWatch and similar:

```json
{"ts":"2026-10-04T12:00:00.123Z","level":"info","source":"Observability.cpp:142","msg":"GET /items 200 0.41ms","method":"GET","path":"/items","status":"200","bytes":"5","duration_ms":"0.41","client_ip":"203.0.113.7","user_agent":"curl/8.5","request_id":"9f2c…","trace_id":"4bf9…"}
```

Each line is written and flushed as it is logged, so logs appear immediately
in `docker logs`, journald or a redirected file, and nothing is lost if the
process crashes.

The level and format are **process-wide**. An `App` applies `log_level` and
`log_format` only when they differ from the defaults (`INFO`, `text`), so
creating an App never resets a level set earlier with
`orbit::utils::Logger::init()` / `set_format()` or by another App. Several Apps
in one process share one setting: the last one that sets it explicitly wins.

Log your own structured events with `orbit::utils::Logger::log_fields(level, __FILE__,
__LINE__, "message", {{"key", "value"}})`. `orbit::utils::Logger::set_sink()` sends
lines somewhere other than stdout (it is called under the logger's lock, so it
must not log itself).

## Access log

`orbit::middleware::access_log()` logs every request when its response is sent:
method, path (with query), status, response bytes, duration, client IP (see
`trusted_proxies()` behind a load balancer), user agent, and the request and
trace IDs when those middlewares run. Streamed responses (`send_headers` /
`write_chunk`) are logged when their headers are sent.

## Request IDs

`orbit::middleware::request_id()` sets `req.request_id` and the `X-Request-ID`
response header. A well-formed incoming ID (up to 128 characters of
`[A-Za-z0-9._:-]`, e.g. from a load balancer) is reused; anything else is
replaced with a random 128-bit ID. Return it in error responses so a user's
report can be matched to the logs.

## Tracing

`orbit::middleware::tracing()` implements [W3C Trace Context](https://www.w3.org/TR/trace-context/):
it continues the trace from an incoming `traceparent` header, or starts a new
one, and sets `req.trace_id` and `req.span_id`. Each finished request is
reported as a `orbit::middleware::Span` (trace and span IDs, parent span, name,
start, duration, status) to `on_span_end`, where it can be handed to an
OpenTelemetry exporter:

```cpp
orbit::middleware::TracingOptions tracing;
tracing.on_span_end = [](const orbit::middleware::Span& span) {
    exporter.enqueue(span);   // keep this fast; it runs on the response path
};
app.use(orbit::middleware::tracing(tracing));
```

When calling other services, pass the context on so their spans join the trace:

```cpp
std::string traceparent = "00-" + req.trace_id + "-" + req.span_id + "-01";
```

## Metrics

`orbit::middleware::Metrics::track()` records, and `app.enable_metrics()` serves in
Prometheus format:

| Metric | Type | Labels |
|---|---|---|
| `orbit_http_requests_total` | counter | `method`, `status` |
| `orbit_http_request_duration_seconds` | histogram (5 ms … 10 s buckets) | `method` |
| `orbit_http_requests_in_flight` | gauge | |
| `orbit_http_request_bytes_total` | counter | |
| `orbit_http_response_bytes_total` | counter | `method` |
| `orbit_active_connections` | gauge | `type` (`tcp`, `quic`) |

Paths are deliberately not a label: every distinct URL would become its own
time series. Metrics are process-wide.
