#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <coroutine>
#include <exception>

namespace orbit::concurrency {

struct Task {
    struct promise_type {
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    Task(std::coroutine_handle<promise_type> h) : handle(h) {}
};

} // namespace concurrency
