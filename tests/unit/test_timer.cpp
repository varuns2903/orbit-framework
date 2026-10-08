#include <gtest/gtest.h>
#include <orbit/server/TimerManager.hpp>
#include <thread>
#include <vector>

using namespace orbit::server;

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


// Connections re-arm their timer on every request (cancel + add). The
// cancelled entries used to stay queued until their old deadline, so the
// queue grew with the request rate: ~400 MB after 30 s of load (#168).
TEST(TimerManagerTest, ReArmingDoesNotGrowTheQueue) {
    TimerManager tm;
    uint64_t id = tm.add_timer(7, std::chrono::seconds(10));
    for (int i = 0; i < 200000; ++i) { // the old queue kept every one
        tm.cancel_timer(id);
        id = tm.add_timer(7, std::chrono::seconds(10));
    }
    EXPECT_EQ(tm.live_timers(), 1u);
    EXPECT_LE(tm.pending_entries(), 2u * 1u + 64u + 1u);
}

TEST(TimerManagerTest, ManyConnectionsReArmingStayBounded) {
    TimerManager tm;
    constexpr int kConnections = 500;
    std::vector<uint64_t> ids(kConnections);
    for (int c = 0; c < kConnections; ++c) ids[c] = tm.add_timer(c, std::chrono::seconds(10));
    for (int round = 0; round < 200; ++round) {
        for (int c = 0; c < kConnections; ++c) {
            tm.cancel_timer(ids[c]);
            ids[c] = tm.add_timer(c, std::chrono::seconds(10));
        }
    }
    EXPECT_EQ(tm.live_timers(), static_cast<size_t>(kConnections));
    EXPECT_LE(tm.pending_entries(), 2u * kConnections + 64u + 1u);
}

TEST(TimerManagerTest, OnlyTheLatestDeadlineOfEachConnectionFires) {
    TimerManager tm;
    // fd 1: re-armed many times, finally to a short timeout -> fires once.
    uint64_t a = tm.add_timer(1, std::chrono::seconds(30));
    for (int i = 0; i < 1000; ++i) {
        tm.cancel_timer(a);
        a = tm.add_timer(1, i == 999 ? std::chrono::milliseconds(5) : std::chrono::seconds(30));
    }
    // fd 2: cancelled for good -> never fires.
    uint64_t b = tm.add_timer(2, std::chrono::milliseconds(5));
    tm.cancel_timer(b);
    // fd 3: a long timer -> must not fire yet.
    tm.add_timer(3, std::chrono::seconds(30));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<int> fired;
    tm.handle_expired_timers([&](int fd) { fired.push_back(fd); });
    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], 1);
    EXPECT_EQ(tm.live_timers(), 1u); // fd 3
}

TEST(TimerManagerTest, NextTimeoutIgnoresCancelledTimers) {
    TimerManager tm;
    uint64_t soon = tm.add_timer(1, std::chrono::milliseconds(1));
    tm.add_timer(2, std::chrono::seconds(10));
    tm.cancel_timer(soon);
    // The cancelled 1 ms timer must not make the loop wake immediately.
    EXPECT_GT(tm.get_next_timeout(), 1000);
    TimerManager empty;
    EXPECT_EQ(empty.get_next_timeout(), -1);
}
