#include <gtest/gtest.h>
#include <orbit/concurrency/ThreadPool.hpp>
#include <atomic>
#include <chrono>

TEST(ThreadPoolTest, ExecutesTasksCorrectly) {
    concurrency::ThreadPool pool(4);
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
