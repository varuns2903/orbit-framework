#include <orbit/middleware/DistributedRateLimiter.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/utils/Logger.hpp>

namespace orbit::middleware {

DistributedRateLimiter::DistributedRateLimiter(const std::string& redis_host, int redis_port, size_t max_requests,
                                               std::chrono::seconds window, KeyFunction key,
                                               bool allow_when_unavailable)
    : max_requests_(max_requests), window_(window), key_(std::move(key)),
      allow_when_unavailable_(allow_when_unavailable),
      redis_(std::make_unique<database::RedisClient>(redis_host, redis_port)) {
    if (!key_) {
        key_ = [](const http::HttpRequest& req) { return req.client_ip; };
    }
}

bool DistributedRateLimiter::operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    const std::string key = "rate:" + key_(req);
    const int window_seconds = static_cast<int>(std::max<std::chrono::seconds::rep>(1, window_.count()));

    long long count = redis_->incr_with_expiry(key, window_seconds);

    if (count <= 0) {
        // Redis is unreachable or returned an error.
        if (allow_when_unavailable_) {
            return true;
        }
        LOG_WARN("Distributed rate limiter: Redis unavailable, rejecting request");
        http::HttpResponse res;
        res.status(http::HttpStatus::ServiceUnavailable);
        res.set_body("503 Service Unavailable");
        res.headers["Retry-After"] = "1";
        writer->send(std::move(res));
        return false;
    }

    if (static_cast<size_t>(count) <= max_requests_) {
        return true;
    }
    
    http::HttpResponse res;
    res.status(http::HttpStatus::TooManyRequests);
    res.set_body("429 Too Many Requests");
    // The window resets at most window_seconds from now.
    res.headers["Retry-After"] = std::to_string(window_seconds);
    writer->send(std::move(res));
    return false;
}

} // namespace middleware
