#include <gtest/gtest.h>
#include <orbit/server/TimerManager.hpp>
#include <thread>

using namespace server;

TEST(TimerManagerTest, BasicTimerExecution) {
    TimerManager tm;
    
    int dummy_fd = 42;
    uint64_t id = tm.add_timer(dummy_fd, std::chrono::milliseconds(10));
    EXPECT_GT(id, 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    
    bool timer_fired = false;
    tm.handle_expired_timers([&](int fd) {
        if (fd == dummy_fd) {
            timer_fired = true;
        }
    });
    
    EXPECT_TRUE(timer_fired);
}

TEST(TimerManagerTest, CancelTimer) {
    TimerManager tm;
    
    int dummy_fd = 99;
    uint64_t id = tm.add_timer(dummy_fd, std::chrono::milliseconds(50));
    tm.cancel_timer(id);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    
    bool timer_fired = false;
    tm.handle_expired_timers([&](int fd) {
        if (fd == dummy_fd) {
            timer_fired = true;
        }
    });
    
    EXPECT_FALSE(timer_fired); // Should not have fired
}

