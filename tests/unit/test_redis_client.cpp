#include <gtest/gtest.h>

#ifdef ORBIT_ENABLE_REDIS
#include <orbit/database/RedisClient.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr int kPort = 6396;

bool start_redis() {
#ifdef _WIN32
    return false;
#else
    if (std::system("command -v redis-server >/dev/null 2>&1") != 0) return false;
    std::string cmd = "redis-server --port " + std::to_string(kPort) +
                      " --save '' --appendonly no --daemonize yes >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
#endif
}

void stop_redis() {
    std::string cmd = "redis-cli -p " + std::to_string(kPort) + " shutdown nosave >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// Runs fn on a detached thread; returns false if it has not finished in time.
// A deadlocked call would otherwise hang the whole test binary.
template <typename Fn>
bool finishes_within(std::chrono::milliseconds limit, Fn fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done, fn]() mutable {
        fn();
        *done = true;
    }).detach();
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!*done && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return *done;
}

} // namespace

TEST(RedisClientTest, ReconnectsAfterServerRestart) {
    if (!start_redis()) GTEST_SKIP() << "redis-server not available";

    // Leaked on purpose: if the fix regresses, a deadlocked thread may still hold it.
    auto* client = new database::RedisClient("127.0.0.1", kPort);
    EXPECT_EQ(client->incr("orbit:test:counter"), 1);

    stop_redis();

    // The first call after the restart discovers the dead socket.
    bool first_done = finishes_within(std::chrono::seconds(5), [client] { client->incr("orbit:test:counter"); });
    ASSERT_TRUE(first_done) << "RedisClient deadlocked after losing its connection";

    ASSERT_TRUE(start_redis());
    long long value = 0;
    bool second_done = finishes_within(std::chrono::seconds(5), [client, &value] { value = client->incr("orbit:test:counter"); });
    ASSERT_TRUE(second_done) << "RedisClient deadlocked while reconnecting";
    EXPECT_EQ(value, 1); // fresh server, fresh counter

    auto got = client->get("orbit:test:missing");
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(client->set("orbit:test:key", "v", 10));
    EXPECT_EQ(client->get("orbit:test:key").value_or(""), "v");

    stop_redis();
    delete client;
}

TEST(RedisClientTest, FailsFastWhenNothingListens) {
    database::RedisClient client("127.0.0.1", 1); // port 1: connection refused
    bool done = finishes_within(std::chrono::seconds(5), [&client] { EXPECT_EQ(client.incr("k"), 0); });
    EXPECT_TRUE(done);
}

#endif // ORBIT_ENABLE_REDIS
