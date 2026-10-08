#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <algorithm>
#include <cmath>

namespace orbit::middleware {

RateLimiter::RateLimiter(RateLimitOptions options)
    : options_(std::move(options)),
      refill_per_second_(static_cast<double>(options_.max_requests) /
                         static_cast<double>(std::max<std::chrono::seconds::rep>(1, options_.window.count()))),
      last_sweep_(Clock::now()) {
    if (!options_.key) {
        options_.key = [](const http::HttpRequest& req) { return req.client_ip; };
    }
    if (options_.max_tracked_clients == 0) options_.max_tracked_clients = 1;
}

RateLimiter::RateLimiter(size_t max_requests, std::chrono::seconds window)
    : RateLimiter([&] {
          RateLimitOptions o;
          o.max_requests = max_requests;
          o.window = window;
          return o;
      }()) {}

size_t RateLimiter::tracked_clients() {
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_.size();
}

// Drops clients whose bucket would be full by now: forgetting them changes
// nothing, since a new bucket also starts full. Caller holds mutex_.
void RateLimiter::evict_refilled(Clock::time_point now) {
    const double capacity = static_cast<double>(options_.max_requests);
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        double elapsed = std::chrono::duration<double>(now - it->second.updated).count();
        if (it->second.tokens + elapsed * refill_per_second_ >= capacity) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
    last_sweep_ = now;
}

bool RateLimiter::operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    const std::string key = options_.key(req);
    const double capacity = static_cast<double>(options_.max_requests);
    double retry_after_seconds = 0;
    bool allowed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = Clock::now();

        // A full sweep is O(clients), so when the map is at capacity it runs at
        // most once a second; otherwise a stream of new keys would sweep each time.
        const auto since_sweep = now - last_sweep_;
        if (since_sweep >= options_.window ||
            (buckets_.size() >= options_.max_tracked_clients && since_sweep >= std::chrono::seconds(1))) {
            evict_refilled(now);
        }
        // Still full of clients that are mid-window: make room rather than grow.
        // Losing a bucket only resets that client to a full allowance.
        while (buckets_.size() >= options_.max_tracked_clients && buckets_.find(key) == buckets_.end()) {
            buckets_.erase(buckets_.begin());
        }

        auto [it, inserted] = buckets_.try_emplace(key, Bucket{capacity, now});
        Bucket& bucket = it->second;
        if (!inserted) {
            double elapsed = std::chrono::duration<double>(now - bucket.updated).count();
            bucket.tokens = std::min(capacity, bucket.tokens + elapsed * refill_per_second_);
            bucket.updated = now;
        }

        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            allowed = true;
        } else if (refill_per_second_ > 0) {
            retry_after_seconds = (1.0 - bucket.tokens) / refill_per_second_;
        } else {
            retry_after_seconds = static_cast<double>(options_.window.count());
        }
    }
    
    if (allowed) {
        return true;
    }

    http::HttpResponse res;
    res.status(http::HttpStatus::TooManyRequests);
    res.set_body("429 Too Many Requests");
    res.headers["Retry-After"] = std::to_string(static_cast<long long>(std::ceil(std::max(1.0, retry_after_seconds))));
    writer->send(std::move(res));
    return false;
}

} // namespace middleware
