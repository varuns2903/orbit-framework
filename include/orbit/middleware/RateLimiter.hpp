#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <functional>
#include <memory>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>

namespace middleware {

/**
 * @brief Settings for the in-memory rate limiter.
 */
struct RateLimitOptions {
    size_t max_requests = 100;                ///< Burst size and requests allowed per window
    std::chrono::seconds window{60};          ///< Window over which max_requests refill
    /// Identifies the client. Defaults to the socket address (req.client_ip).
    /// Behind a reverse proxy you trust, return the address it forwards
    /// instead; never trust client-supplied headers otherwise.
    std::function<std::string(const http::HttpRequest&)> key;
    size_t max_tracked_clients = 100000;      ///< Upper bound on remembered clients
};

/**
 * @brief Token-bucket rate limiter keyed by client.
 *
 * Each client gets max_requests tokens that refill continuously over window.
 * Rejected requests receive 429 with Retry-After. Clients whose bucket has
 * refilled are forgotten, and at most max_tracked_clients are kept, so the
 * memory used is bounded no matter how many addresses a client rotates through.
 */
class RateLimiter {
public:
    explicit RateLimiter(RateLimitOptions options);
    RateLimiter(size_t max_requests, std::chrono::seconds window);
    
    bool operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer);

    /// Number of clients currently tracked (for tests and metrics).
    size_t tracked_clients();

private:
    using Clock = std::chrono::steady_clock;

    struct Bucket {
        double tokens;
        Clock::time_point updated;
    };

    void evict_refilled(Clock::time_point now);

    RateLimitOptions options_;
    double refill_per_second_;
    std::unordered_map<std::string, Bucket> buckets_;
    Clock::time_point last_sweep_;
    std::mutex mutex_;
};

inline auto rate_limit(RateLimitOptions options) {
    return [limiter = std::make_shared<RateLimiter>(std::move(options))](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*limiter)(req, writer);
    };
}

inline auto rate_limit(size_t max_requests, std::chrono::seconds window) {
    RateLimitOptions options;
    options.max_requests = max_requests;
    options.window = window;
    return rate_limit(std::move(options));
}

} // namespace middleware
