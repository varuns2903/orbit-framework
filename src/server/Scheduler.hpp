#pragma once
#include <orbit/server/Timer.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace orbit::server {

namespace detail {

struct TimerState {
    std::function<void()> callback;
    std::chrono::milliseconds delay{0};    // before the first run
    std::chrono::milliseconds interval{0}; // between runs; zero runs once
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};     // a one-shot timer has run
    std::atomic<bool> running{false};      // a run is in progress (no overlapping runs)
};

} // namespace detail

/**
 * @brief Runs App timers: one thread keeps the deadlines and hands each due
 *        callback to a dispatcher (the App's worker pool).
 *
 * Timers added before start() begin counting at start(). stop() ends the
 * thread; queued timers are kept for the next start().
 */
class Scheduler {
public:
    using Dispatch = std::function<void(std::function<void()>)>;

    Scheduler() = default;
    ~Scheduler() { stop(); }
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    TimerHandle add(std::chrono::milliseconds delay, std::chrono::milliseconds interval, std::function<void()> callback);

    void start(Dispatch dispatch);
    void stop();

private:
    using Clock = std::chrono::steady_clock;
    void run();
    void schedule_locked(Clock::time_point due, std::shared_ptr<detail::TimerState> state);

    std::mutex mutex_;
    std::condition_variable wake_;
    std::multimap<Clock::time_point, std::shared_ptr<detail::TimerState>> due_;
    std::vector<std::shared_ptr<detail::TimerState>> not_started_; // added before start()
    Dispatch dispatch_;
    bool running_ = false;
    std::thread thread_;
};

} // namespace orbit::server
