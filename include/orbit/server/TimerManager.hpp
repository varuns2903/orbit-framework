#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <chrono>
#include <functional>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>

namespace orbit::server {

using TimePoint = std::chrono::steady_clock::time_point;

struct TimerEvent {
    TimePoint expiration;
    uint64_t timer_id;
    int fd;
    
    // Min-heap ordering: lowest expiration time comes first
    bool operator>(const TimerEvent& other) const {
        return expiration > other.expiration;
    }
};

/**
 * @brief Manages timers for connection timeouts.
 */
class TimerManager {
public:
    /**
     * @brief Constructs a TimerManager.
     */
    TimerManager();

    /**
     * @brief Adds a timer for a given file descriptor.
     * @param fd The file descriptor.
     * @param timeout The timeout duration.
     * @return The timer ID.
     */
    uint64_t add_timer(int fd, std::chrono::milliseconds timeout);
    
    /**
     * @brief Cancels a timer by ID.
     * @param timer_id The timer ID.
     */
    void cancel_timer(uint64_t timer_id);

    /**
     * @brief Gets the time remaining until the next timeout.
     * @return Milliseconds until the next timeout, or -1 if none.
     */
    int get_next_timeout();

    /**
     * @brief Processes all expired timers.
     * @param on_timeout Callback invoked for each expired timer with its file descriptor.
     */
    void handle_expired_timers(std::function<void(int)> on_timeout);

private:
    std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<TimerEvent>> timers_;
    std::unordered_set<uint64_t> cancelled_timers_;
    uint64_t next_timer_id_{1};
    std::mutex mutex_;
};

} // namespace server
