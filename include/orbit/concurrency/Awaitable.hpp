#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orbit::concurrency {

template <typename T = void>
class Awaitable;

namespace detail {

template <typename T>
struct AwaitableResult {
    template <typename U>
        requires std::is_convertible_v<U&&, T>
    void return_value(U&& value) {
        value_.emplace(std::forward<U>(value));
    }
    T take() {
        if (error_) std::rethrow_exception(error_);
        return std::move(*value_);
    }

    std::optional<T> value_;
    std::exception_ptr error_;
};

template <>
struct AwaitableResult<void> {
    void return_void() noexcept {}
    void take() {
        if (error_) std::rethrow_exception(error_);
    }

    std::exception_ptr error_;
};

} // namespace detail

/**
 * @brief A lazy coroutine that produces a T for whoever co_awaits it.
 *
 * For helpers called from a handler (or from another Awaitable), so a
 * handler need not be one long coroutine:
 *
 * @code
 * Awaitable<std::optional<User>> load_user(std::shared_ptr<PostgresClient> db, int id) {
 *     auto users = co_await query_User(db).where(Col("id") == id).get_async();
 *     if (users.empty()) co_return std::nullopt;
 *     co_return users.front();
 * }
 *
 * Task show_user(HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
 *     auto user = co_await load_user(db, std::stoi(req.params.at("id")));
 *     HttpResponse res;
 *     if (user) res.json(*user); else res.status(HttpStatus::NotFound);
 *     w->send(std::move(res));
 * }
 * @endcode
 *
 * - Nothing runs until it is awaited; awaiting starts it on the awaiting
 *   thread, and the awaiting coroutine resumes, by symmetric transfer, on
 *   whichever thread the helper finishes on.
 * - An exception thrown in the helper is rethrown at the co_await. In a
 *   handler that is not caught, it reaches on_error (else a 500), exactly as
 *   if the handler had thrown it.
 * - Await it once. Destroying it without awaiting simply never runs it.
 * - Handlers themselves keep returning Task: the router starts a Task, but
 *   would never await an Awaitable.
 */
template <typename T>
class [[nodiscard]] Awaitable {
public:
    struct promise_type : detail::AwaitableResult<T> {
        std::coroutine_handle<> continuation = std::noop_coroutine();

        Awaitable get_return_object() noexcept {
            return Awaitable(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void unhandled_exception() noexcept { this->error_ = std::current_exception(); }
    };

    Awaitable(Awaitable&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Awaitable& operator=(Awaitable&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Awaitable(const Awaitable&) = delete;
    Awaitable& operator=(const Awaitable&) = delete;
    ~Awaitable() {
        if (handle_) handle_.destroy();
    }

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle_.promise().continuation = awaiting;
        return handle_;
    }
    T await_resume() {
        if (!handle_) throw std::logic_error("co_await on an empty (moved-from) Awaitable");
        return handle_.promise().take();
    }

private:
    explicit Awaitable(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    std::coroutine_handle<promise_type> handle_;
};

} // namespace orbit::concurrency
