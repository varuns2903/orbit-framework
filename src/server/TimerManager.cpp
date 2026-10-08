#include <orbit/server/TimerManager.hpp>

namespace orbit::server {

namespace {
// The queue may hold this many entries beyond twice the live timers before
// it is rebuilt; keeps tiny queues from being compacted constantly.
constexpr size_t kCompactionSlack = 64;
} // namespace

TimerManager::TimerManager() = default;

uint64_t TimerManager::add_timer(int fd, std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = ++next_timer_id_;
    TimePoint expiration = std::chrono::steady_clock::now() + timeout;

    live_.emplace(id, LiveTimer{fd, expiration});
    timers_.push({expiration, id, fd});
    compact_if_needed_locked();

    return id;
}

void TimerManager::cancel_timer(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The queue entry stays until it reaches the front or the queue is
    // compacted; only live_ decides whether a timer still counts.
    live_.erase(timer_id);
    compact_if_needed_locked();
}

void TimerManager::drop_cancelled_front_locked() {
    while (!timers_.empty() && live_.find(timers_.top().timer_id) == live_.end()) {
        timers_.pop();
    }
}

void TimerManager::compact_if_needed_locked() {
    if (timers_.size() <= 2 * live_.size() + kCompactionSlack) return;
    std::vector<TimerEvent> entries;
    entries.reserve(live_.size());
    for (const auto& [id, timer] : live_) {
        entries.push_back({timer.expiration, id, timer.fd});
    }
    timers_ = decltype(timers_)(std::greater<TimerEvent>(), std::move(entries));
}

int TimerManager::get_next_timeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    // A cancelled entry at the front would wake the loop for nothing.
    drop_cancelled_front_locked();
    if (timers_.empty()) {
        return -1; // Infinite timeout
    }

    auto now = std::chrono::steady_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(timers_.top().expiration - now).count();

    if (diff < 0) return 0; // Already expired! Wake up immediately
    return static_cast<int>(diff);
}

void TimerManager::handle_expired_timers(std::function<void(int)> on_timeout) {
    std::vector<int> expired_fds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        while (!timers_.empty()) {
            const TimerEvent top = timers_.top();
            if (top.expiration > now) {
                break; // No more expired timers
            }
            timers_.pop();

            auto it = live_.find(top.timer_id);
            if (it != live_.end()) {
                expired_fds.push_back(it->second.fd);
                live_.erase(it);
            }
        }
    }

    // Call callbacks outside the lock to prevent deadlocks!
    for (int fd : expired_fds) {
        on_timeout(fd);
    }
}

size_t TimerManager::pending_entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    return timers_.size();
}

size_t TimerManager::live_timers() {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_.size();
}

} // namespace server
