#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <memory>

namespace orbit::server {

namespace detail { struct TimerState; }

/**
 * @brief A timer from App::run_every() or App::run_after().
 *
 * Copies refer to the same timer. Dropping every copy does not cancel it;
 * call cancel(). A default-constructed handle refers to no timer.
 */
class TimerHandle {
public:
    TimerHandle() = default;

    /// Stops the timer: no further runs start (one already running finishes).
    void cancel();
    /// True until cancelled, or until a run_after() timer has run.
    bool active() const;

private:
    friend class Scheduler;
    explicit TimerHandle(std::shared_ptr<detail::TimerState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::TimerState> state_;
};

} // namespace orbit::server
