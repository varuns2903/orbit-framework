#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <chrono>
#include <functional>
#include <vector>
#include <queue>
#include <unordered_map>
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

    /// Entries held in the deadline queue, live or cancelled. Bounded by a
    /// small multiple of the live timers however often they are re-armed;
    /// exposed for tests and diagnostics.
    size_t pending_entries();

    /// Timers that have been added and neither fired nor been cancelled.
    size_t live_timers();

private:
    struct LiveTimer {
        int fd;
        TimePoint expiration;
    };

    // Drops cancelled entries from the front of the queue.
    void drop_cancelled_front_locked();
    // Rebuilds the queue from the live timers once cancelled entries
    // dominate it. Connections re-arm their timer on every request, and the
    // cancelled entries used to stay queued until their old deadline
    // (10-30 s), so the queue grew with the request rate (#168).
    void compact_if_needed_locked();

    std::priority_queue<TimerEvent, std::vector<TimerEvent>, std::greater<TimerEvent>> timers_;
    std::unordered_map<uint64_t, LiveTimer> live_;
    uint64_t next_timer_id_{1};
    std::mutex mutex_;
};

} // namespace server
