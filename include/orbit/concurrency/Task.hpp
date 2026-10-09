#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/concurrency/Awaitable.hpp> // helpers that handlers co_await
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <type_traits>

namespace orbit::http { class ResponseWriter; struct HttpRequest; }

namespace orbit::concurrency {

namespace detail {
// Defined in src/concurrency/Task.cpp, so this header needs no HTTP headers.
uint64_t writer_generation(const std::shared_ptr<http::ResponseWriter>& writer) noexcept;
std::shared_ptr<void> keep_request_alive(const http::HttpRequest& request) noexcept;
void report_task_exception(const std::shared_ptr<http::ResponseWriter>& writer, uint64_t generation,
                           std::exception_ptr error) noexcept;
} // namespace detail

/**
 * @brief Fire-and-forget coroutine type, used as the return type of
 *        coroutine route handlers.
 *
 * An exception that escapes the coroutine never terminates the process.
 * In a handler — a coroutine that takes a `std::shared_ptr<ResponseWriter>`
 * parameter — it is handled like an exception from a synchronous handler:
 * the router's on_error handler runs, else the client gets a 500. Elsewhere
 * it is logged.
 *
 * A request the coroutine takes as a parameter (`HttpRequest&`) stays valid,
 * headers and body included, until the coroutine finishes: across every
 * co_await, and even after the response has been sent.
 */
struct Task {
    struct promise_type {
        promise_type() = default;

        // The compiler passes the coroutine's parameters to the promise's
        // constructor; that is how a handler's ResponseWriter and request
        // are found.
        template <typename First, typename... Rest>
        explicit promise_type(First& first, Rest&... rest) {
            capture(first);
            (capture(rest), ...);
        }

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() {}
        void unhandled_exception() noexcept {
            detail::report_task_exception(writer_, generation_, std::current_exception());
        }

    private:
        template <typename T>
        void capture(T& value) {
            if constexpr (std::is_same_v<std::remove_cv_t<T>, std::shared_ptr<http::ResponseWriter>>) {
                if (!writer_) {
                    writer_ = value;
                    generation_ = detail::writer_generation(writer_);
                }
            } else if constexpr (std::is_same_v<std::remove_cv_t<T>, http::HttpRequest>) {
                if (!request_) request_ = detail::keep_request_alive(value);
            }
        }

        std::shared_ptr<http::ResponseWriter> writer_;
        uint64_t generation_ = 0;
        std::shared_ptr<void> request_; // the request's storage, held until the coroutine ends (#200)
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
};

} // namespace concurrency
