#pragma once
#include <string>
#include <chrono>
#include <functional>
#include <memory>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/database/RedisClient.hpp>

namespace middleware {

/**
 * @ingroup middlewares
 * @brief Fixed-window rate limiter shared across instances through Redis.
 *
 * Each client may make max_requests requests per window. The counter and its
 * expiry are created atomically, so a client can never be locked out by a
 * counter that never expires. Rejected requests receive 429 with Retry-After.
 */
class DistributedRateLimiter {
public:
    /// Identifies the client; defaults to req.client_ip (see RateLimitOptions::key).
    using KeyFunction = std::function<std::string(const http::HttpRequest&)>;

    /**
     * @brief Constructs a new Distributed Rate Limiter.
     *
     * @param redis_host The Redis server host.
     * @param redis_port The Redis server port.
     * @param max_requests Maximum number of requests allowed in the time window.
     * @param window The time window for rate limiting.
     * @param key Maps a request to the client identity; defaults to req.client_ip.
     * @param allow_when_unavailable If Redis cannot be reached, let requests
     *        through (fail open) instead of rejecting them with 503 (fail closed).
     */
    DistributedRateLimiter(const std::string& redis_host, int redis_port, size_t max_requests,
                           std::chrono::seconds window, KeyFunction key = nullptr,
                           bool allow_when_unavailable = false);

    /**
     * @brief Middleware execution operator.
     *
     * @return true If the request is allowed.
     * @return false If the request was rejected (429 or 503).
     */
    bool operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer);

private:
    size_t max_requests_;
    std::chrono::seconds window_;
    KeyFunction key_;
    bool allow_when_unavailable_;
    std::unique_ptr<database::RedisClient> redis_;
};

/// Creates a DistributedRateLimiter middleware; see its constructor for parameters.
inline auto distributed_rate_limit(const std::string& redis_host, int redis_port, size_t max_requests,
                                   std::chrono::seconds window,
                                   DistributedRateLimiter::KeyFunction key = nullptr,
                                   bool allow_when_unavailable = false) {
    return [limiter = std::make_shared<DistributedRateLimiter>(redis_host, redis_port, max_requests, window,
                                                               std::move(key), allow_when_unavailable)]
           (http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*limiter)(req, writer);
    };
}

} // namespace middleware
