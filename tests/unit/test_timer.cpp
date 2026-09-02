#include <gtest/gtest.h>
#include <orbit/server/TimerManager.hpp>
#include <orbit/network/Proactor.hpp>
#include <thread>

using namespace server;

TEST(TimerManagerTest, BasicTimerExecution) {
    network::Proactor proactor(10);
    TimerManager tm(proactor);
    
    bool timer_fired = false;
    uint64_t id = tm.add_timer(std::chrono::milliseconds(10), [&]() {
        timer_fired = true;
    });
    EXPECT_GT(id, 0);
    
    // Process proactor for a short while
    auto start = std::chrono::steady_clock::now();
    while (!timer_fired && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
        proactor.poll(10);
    }
    
    EXPECT_TRUE(timer_fired);
}

TEST(TimerManagerTest, CancelTimer) {
    network::Proactor proactor(10);
    TimerManager tm(proactor);
    
    bool timer_fired = false;
    uint64_t id = tm.add_timer(std::chrono::milliseconds(50), [&]() {
        timer_fired = true;
    });
    
    tm.cancel_timer(id);
    
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100)) {
        proactor.poll(10);
    }
    
    EXPECT_FALSE(timer_fired); // Should not have fired
}
