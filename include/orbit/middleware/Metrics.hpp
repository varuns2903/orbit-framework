#pragma once
#include <orbit/routing/Router.hpp>

namespace middleware {

/**
 * @ingroup middlewares
 * @brief Middleware for tracking HTTP request metrics.
 */
class Metrics {
public:
    /**
     * @brief Returns a middleware handler that tracks requests:
     *
     * - `orbit_http_requests_total{method,status}` (counter)
     * - `orbit_http_request_duration_seconds{method}` (histogram)
     * - `orbit_http_requests_in_flight` (gauge)
     * - `orbit_http_request_bytes_total` / `orbit_http_response_bytes_total` (counters)
     *
     * Serve them with App::enable_metrics(). WebSocket upgrades are not counted.
     *
     * @return routing::Middleware The metrics tracking middleware handler.
     */
    static routing::Middleware track();
};

} // namespace middleware
