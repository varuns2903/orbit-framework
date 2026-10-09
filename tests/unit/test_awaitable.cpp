#include <gtest/gtest.h>
#include <orbit/concurrency/Awaitable.hpp>
#include <orbit/concurrency/Task.hpp>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Awaitable<T>: lazy helpers that a handler (a Task) co_awaits (#200).

using orbit::concurrency::Awaitable;
using orbit::concurrency::Task;

namespace {

// Suspends until the test resumes it by hand, standing in for I/O that
// completes later.
struct Pending {
    std::coroutine_handle<>* slot;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const noexcept { *slot = h; }
    void await_resume() const noexcept {}
};

Awaitable<int> answer() { co_return 42; }

Awaitable<std::string> greet(std::string name) { co_return "hello " + name; }

Awaitable<int> add_later(std::coroutine_handle<>* slot, int a, int b) {
    co_await Pending{slot};
    co_return a + b;
}

Awaitable<int> twice(std::coroutine_handle<>* slot, int x) {
    int once = co_await add_later(slot, x, 0);
    co_return once * 2;
}

Awaitable<void> fail() {
    throw std::runtime_error("helper failed");
    co_return;
}

Awaitable<std::unique_ptr<int>> move_only() { co_return std::make_unique<int>(7); }

} // namespace

TEST(AwaitableTest, ReturnsValues) {
    int n = 0;
    std::string s;
    std::unique_ptr<int> p;
    [](int& n, std::string& s, std::unique_ptr<int>& p) -> Task {
        n = co_await answer();
        s = co_await greet("orbit");
        p = co_await move_only();
    }(n, s, p);
    EXPECT_EQ(n, 42);
    EXPECT_EQ(s, "hello orbit");
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 7);
}

TEST(AwaitableTest, IsLazy) {
    bool ran = false;
    auto make = [](bool& ran) -> Awaitable<void> {
        ran = true;
        co_return;
    };
    {
        auto pending = make(ran);
        EXPECT_FALSE(ran) << "an Awaitable runs only when awaited";
    }
    EXPECT_FALSE(ran) << "destroying it unawaited never runs it";
    [](Awaitable<void> a) -> Task { co_await a; }(make(ran));
    EXPECT_TRUE(ran);
}

// Nested helpers that really suspend: the outer Task resumes, through both
// levels, when the innermost operation completes.
TEST(AwaitableTest, ResumesTheAwaiterWhenTheHelperCompletesLater) {
    std::coroutine_handle<> slot;
    int result = -1;
    [](std::coroutine_handle<>* slot, int& result) -> Task {
        result = co_await twice(slot, 21);
    }(&slot, result);
    EXPECT_EQ(result, -1) << "still waiting";
    ASSERT_TRUE(slot);
    slot.resume();
    EXPECT_EQ(result, 42);
}

TEST(AwaitableTest, ExceptionsReachTheAwaiter) {
    std::string caught;
    [](std::string& caught) -> Task {
        try {
            co_await fail();
        } catch (const std::exception& e) {
            caught = e.what();
        }
    }(caught);
    EXPECT_EQ(caught, "helper failed");
}

TEST(AwaitableTest, AwaitingAMovedFromAwaitableThrows) {
    std::string caught;
    [](std::string& caught) -> Task {
        auto a = answer();
        auto b = std::move(a);
        try {
            co_await a;
        } catch (const std::logic_error& e) {
            caught = e.what();
        }
        co_await b;
    }(caught);
    EXPECT_NE(caught.find("moved-from"), std::string::npos) << caught;
}

// A long chain of helpers that finish synchronously must not grow the stack
// (symmetric transfer).
TEST(AwaitableTest, DeepSynchronousChainsDoNotOverflow) {
    struct Chain {
        static Awaitable<int> step(int n) {
            if (n == 0) co_return 0;
            co_return 1 + co_await step(n - 1);
        }
    };
    int depth = 0;
    [](int& depth) -> Task { depth = co_await Chain::step(10000); }(depth);
    EXPECT_EQ(depth, 10000);
}
