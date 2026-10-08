#include <gtest/gtest.h>
#include <orbit/concurrency/ThreadPool.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

TEST(ThreadPoolTest, ExecutesTasksCorrectly) {
    orbit::concurrency::ThreadPool pool(4);
    std::atomic<int> counter{0};
    
    for (int i = 0; i < 100; ++i) {
        pool.enqueue([&counter] {
            counter++;
        });
    }
    
    // Give it a tiny amount of time to finish processing 100 simple increments across 4 threads
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    EXPECT_EQ(counter.load(), 100);
}

// The destructor used to set stop_ without holding the queue mutex, so a
// worker could miss the wake-up and join() never returned. Creating and
// destroying pools back to back hit it within a few seconds.
TEST(ThreadPoolTest, DestructorDoesNotMissTheStopSignal) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done] {
        for (int i = 0; i < 20000; ++i) {
            orbit::concurrency::ThreadPool pool(1);
        }
        *done = true;
    }).detach(); // detached: if it hangs, fail the test instead of the binary
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!*done && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(*done) << "a ThreadPool destructor never returned";
}
