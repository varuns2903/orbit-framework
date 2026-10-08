#include <orbit/server/TimerManager.hpp>

namespace orbit::server {

TimerManager::TimerManager() = default;

uint64_t TimerManager::add_timer(int fd, std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = ++next_timer_id_;
    TimePoint expiration = std::chrono::steady_clock::now() + timeout;
    
    timers_.push({expiration, id, fd});
    
    return id;
}

void TimerManager::cancel_timer(uint64_t timer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_timers_.insert(timer_id);
}

int TimerManager::get_next_timeout() {
    std::lock_guard<std::mutex> lock(mutex_);
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
            auto top = timers_.top();
            
            if (top.expiration > now) {
                break; // No more expired timers
            }
            
            timers_.pop();
            
            if (cancelled_timers_.find(top.timer_id) == cancelled_timers_.end()) {
                expired_fds.push_back(top.fd);
            } else {
                cancelled_timers_.erase(top.timer_id);
            }
        }
    }
    
    // Call callbacks outside the lock to prevent deadlocks!
    for (int fd : expired_fds) {
        on_timeout(fd);
    }
}

} // namespace server
