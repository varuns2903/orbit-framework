#include "Scheduler.hpp"
#include <orbit/utils/Logger.hpp>
#include <exception>

namespace orbit::server {

void TimerHandle::cancel() {
    if (state_) state_->cancelled = true;
}

bool TimerHandle::active() const {
    return state_ && !state_->cancelled && !state_->finished;
}

TimerHandle Scheduler::add(std::chrono::milliseconds delay, std::chrono::milliseconds interval,
                           std::function<void()> callback) {
    auto state = std::make_shared<detail::TimerState>();
    state->callback = std::move(callback);
    state->delay = delay < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : delay;
    state->interval = interval < std::chrono::milliseconds(0) ? std::chrono::milliseconds(0) : interval;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            schedule_locked(Clock::now() + state->delay, state);
        } else {
            not_started_.push_back(state);
        }
    }
    wake_.notify_all();
    return TimerHandle(state);
}

void Scheduler::schedule_locked(Clock::time_point due, std::shared_ptr<detail::TimerState> state) {
    due_.emplace(due, std::move(state));
}

void Scheduler::start(Dispatch dispatch) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        running_ = true;
        dispatch_ = std::move(dispatch);
        const auto now = Clock::now();
        for (auto& state : not_started_) {
            const auto due = now + state->delay; // before the move below: argument order is unspecified
            schedule_locked(due, std::move(state));
        }
        not_started_.clear();
    }
    thread_ = std::thread([this] { run(); });
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
        // Keep the timers for a later start(), counting again from there.
        for (auto& [due, state] : due_) not_started_.push_back(std::move(state));
        due_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    dispatch_ = nullptr;
}

void Scheduler::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
        if (due_.empty()) {
            wake_.wait(lock);
            continue;
        }
        auto first = due_.begin();
        if (Clock::now() < first->first) {
            wake_.wait_until(lock, first->first);
            continue;
        }
        const auto due = first->first;
        std::shared_ptr<detail::TimerState> state = std::move(first->second);
        due_.erase(first);
        if (state->cancelled) continue;

        if (state->interval.count() > 0) {
            // Fixed rate from the original schedule; a tick that would land in
            // the past (the loop or the pool fell behind) moves to now.
            auto next = due + state->interval;
            if (next < Clock::now()) next = Clock::now() + state->interval;
            schedule_locked(next, state);
        }
        // A run still in progress makes this tick a no-op: runs never overlap.
        bool expected = false;
        if (!state->running.compare_exchange_strong(expected, true)) continue;
        Dispatch dispatch = dispatch_;
        lock.unlock();
        auto job = [state] {
            try {
                if (!state->cancelled) state->callback();
            } catch (const std::exception& e) {
                LOG_ERROR("Timer callback threw: " << e.what());
            } catch (...) {
                LOG_ERROR("Timer callback threw");
            }
            if (state->interval.count() == 0) state->finished = true;
            state->running = false;
        };
        try {
            dispatch(std::move(job));
        } catch (...) {
            state->running = false;
            LOG_ERROR("Could not dispatch a timer callback");
        }
        lock.lock();
    }
}

} // namespace orbit::server
