#include <orbit/middleware/Metrics.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <chrono>

namespace middleware {

namespace {

const char* method_label(http::HttpMethod m) {
    switch (m) {
        case http::HttpMethod::GET: return "GET";
        case http::HttpMethod::POST: return "POST";
        case http::HttpMethod::PUT: return "PUT";
        case http::HttpMethod::PATCH: return "PATCH";
        case http::HttpMethod::DELETE: return "DELETE";
        case http::HttpMethod::OPTIONS: return "OPTIONS";
        case http::HttpMethod::HEAD: return "HEAD";
        default: return "UNKNOWN";
    }
}

} // namespace

routing::Middleware Metrics::track() {
    return [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> res_writer) -> bool {
        // A WebSocket upgrade never sends an HttpResponse through the writer,
        // so it would stay "in flight" forever.
        if (req.headers.find("Upgrade") != req.headers.end()) return true;

        auto& registry = utils::PrometheusRegistry::get_instance();
        const std::string method = std::string("method=\"") + method_label(req.method) + "\"";
        registry.inc_gauge("orbit_http_requests_in_flight");
        registry.inc_counter("orbit_http_request_bytes_total", "", static_cast<double>(req.body.size()));

        auto start = std::chrono::steady_clock::now();
        res_writer->add_interceptor([start, method](http::HttpResponse& res) {
            auto& reg = utils::PrometheusRegistry::get_instance();
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            reg.dec_gauge("orbit_http_requests_in_flight");
            reg.inc_counter("orbit_http_requests_total",
                            method + ",status=\"" + std::to_string(static_cast<int>(res.status_code)) + "\"");
            reg.observe_histogram("orbit_http_request_duration_seconds", method, seconds);
            size_t bytes = res.body.size();
            auto cl = res.headers.find("Content-Length");
            if (cl != res.headers.end()) {
                try {
                    bytes = static_cast<size_t>(std::stoull(cl->second));
                } catch (...) {
                }
            }
            reg.inc_counter("orbit_http_response_bytes_total", method, static_cast<double>(bytes));
        });
        return true;
    };
}

} // namespace middleware
