#include <orbit/middleware/Observability.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/utils/Random.hpp>

#include <cstdio>

namespace middleware {

namespace {

const char* method_name(http::HttpMethod m) {
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

bool is_hex(std::string_view s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool all_zero(std::string_view s) {
    return s.find_first_not_of('0') == std::string_view::npos;
}

bool is_safe_request_id(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == ':';
        if (!ok) return false;
    }
    return true;
}

// Bytes in the response body: Content-Length when set (file and streamed
// responses), otherwise the in-memory body.
size_t response_bytes(const http::HttpResponse& res) {
    auto it = res.headers.find("Content-Length");
    if (it != res.headers.end()) {
        try {
            return static_cast<size_t>(std::stoull(it->second));
        } catch (...) {
        }
    }
    return res.body.size();
}

} // namespace

routing::Middleware request_id(RequestIdOptions options) {
    return [options](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        std::string id;
        if (options.trust_incoming) {
            auto it = req.headers.find(options.header);
            if (it != req.headers.end() && is_safe_request_id(it->second)) id = std::string(it->second);
        }
        if (id.empty()) id = utils::secure_random_hex(16);
        req.request_id = id;
        writer->set_header(options.header, id);
        return true;
    };
}

routing::Middleware tracing(TracingOptions options) {
    auto on_span_end = std::make_shared<std::function<void(const Span&)>>(std::move(options.on_span_end));
    return [on_span_end](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        // traceparent: version-traceid-parentid-flags (W3C Trace Context).
        std::string parent;
        auto it = req.headers.find("traceparent");
        if (it != req.headers.end()) {
            std::string_view tp = it->second;
            if (tp.size() >= 55 && tp[2] == '-' && tp[35] == '-' && tp[52] == '-' && tp.substr(0, 2) != "ff") {
                std::string_view trace = tp.substr(3, 32);
                std::string_view span = tp.substr(36, 16);
                if (is_hex(trace, 32) && is_hex(span, 16) && !all_zero(trace) && !all_zero(span)) {
                    req.trace_id = std::string(trace);
                    parent = std::string(span);
                }
            }
        }
        if (req.trace_id.empty()) req.trace_id = utils::secure_random_hex(16);
        req.span_id = utils::secure_random_hex(8);

        if (!*on_span_end) return true;
        auto span = std::make_shared<Span>();
        span->trace_id = req.trace_id;
        span->span_id = req.span_id;
        span->parent_span_id = parent;
        span->name = std::string(method_name(req.method)) + " " + req.uri;
        span->start = std::chrono::system_clock::now();
        span->client_ip = req.client_ip;
        auto started = std::chrono::steady_clock::now();
        writer->add_interceptor([span, started, on_span_end](http::HttpResponse& res) {
            span->duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
            span->status = static_cast<int>(res.status_code);
            (*on_span_end)(*span);
        });
        return true;
    };
}

routing::Middleware access_log(AccessLogOptions options) {
    return [options](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        if (utils::Logger::current_level > options.level) return true;
        auto started = std::chrono::steady_clock::now();
        // Copied now: the request may be gone when the response is sent.
        std::string method = method_name(req.method);
        std::string path = req.target.empty() ? req.uri : req.target;
        std::string client_ip = req.client_ip;
        auto ua_it = req.headers.find("User-Agent");
        std::string user_agent = ua_it != req.headers.end() ? std::string(ua_it->second) : "";
        // Register request_id() and tracing() before access_log(): the IDs
        // are captured here, as the request may be gone when the response
        // is sent.
        std::string request_id = req.request_id;
        std::string trace_id = req.trace_id;
        utils::LogLevel level = options.level;

        writer->add_interceptor([=](http::HttpResponse& res) {
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            int status = static_cast<int>(res.status_code);
            size_t bytes = response_bytes(res);
            char duration[32];
            std::snprintf(duration, sizeof(duration), "%.2f", ms);

            utils::LogFields fields = {
                {"method", method},       {"path", path},         {"status", std::to_string(status)},
                {"bytes", std::to_string(bytes)}, {"duration_ms", duration}, {"client_ip", client_ip},
            };
            if (!user_agent.empty()) fields.emplace_back("user_agent", user_agent);
            if (!request_id.empty()) fields.emplace_back("request_id", request_id);
            if (!trace_id.empty()) fields.emplace_back("trace_id", trace_id);
            utils::Logger::log_fields(level, __FILE__, __LINE__,
                                      method + " " + path + " " + std::to_string(status) + " " + duration + "ms",
                                      fields);
        });
        return true;
    };
}

} // namespace middleware
