#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include "../utils/TestConfig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// App lifecycle hooks and timers (#205): on_start / on_stop, run_every /
// run_after.

namespace {

constexpr uint16_t kPort = 8178;
using namespace std::chrono_literals;

bool reachable() {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    orbit::network::close_socket(fd);
    return ok;
}

// Polls @p done for up to @p limit.
template <typename F>
bool eventually(F done, std::chrono::milliseconds limit = 3000ms) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return done();
}

class Server {
public:
    explicit Server(std::function<void(orbit::server::App&)> configure) {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<orbit::server::App>(cfg);
        configure(*app);
        thread = std::thread([this] { app->listen(); });
        eventually([] { return reachable(); });
    }
    void stop() {
        if (stopped) return;
        stopped = true;
        app->stop();
        if (thread.joinable()) thread.join();
    }
    ~Server() { stop(); }

    std::unique_ptr<orbit::server::App> app;
    std::thread thread;
    bool stopped = false;
};

} // namespace

TEST(LifecycleTest, OnStartRunsOnceTheServerIsUp) {
    std::atomic<int> runs{0};
    std::atomic<bool> was_reachable{false};
    std::atomic<bool> got_app{false};
    std::vector<int> order;
    std::mutex mu;
    Server server([&](orbit::server::App& app) {
        app.on_start([&](orbit::server::App& a) {
            got_app = &a == &app;
            // Listeners are bound before hooks run.
            was_reachable = eventually([] { return reachable(); }, 1000ms);
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(1);
            ++runs;
        });
        app.on_start([&](orbit::server::App&) {
            std::lock_guard<std::mutex> lock(mu);
            order.push_back(2);
        });
    });
    ASSERT_TRUE(eventually([&] { return runs.load() == 1; }));
    EXPECT_TRUE(got_app);
    EXPECT_TRUE(was_reachable);
    ASSERT_TRUE(eventually([&] {
        std::lock_guard<std::mutex> lock(mu);
        return order.size() == 2;
    }));
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(LifecycleTest, OnStopRunsOnceWhenTheServerStops) {
    std::atomic<int> stops{0};
    {
        Server server([&](orbit::server::App& app) {
            app.on_stop([&](orbit::server::App&) { ++stops; });
            app.on_stop([](orbit::server::App&) { throw std::runtime_error("logged, not fatal"); });
        });
        EXPECT_EQ(stops, 0);
        server.app->shutdown(std::chrono::seconds(1));
        if (server.thread.joinable()) server.thread.join();
        server.stopped = true;
        EXPECT_EQ(stops, 1);
        server.app->stop(); // a second stop runs nothing more
    }
    EXPECT_EQ(stops, 1);

    // An App that never started has nothing to stop.
    std::atomic<int> never{0};
    {
        orbit::server::App app(orbit::test::server_config());
        app.on_stop([&](orbit::server::App&) { ++never; });
        app.stop();
    }
    EXPECT_EQ(never, 0);
}

TEST(LifecycleTest, RunEveryTicksUntilCancelledOrStopped) {
    std::atomic<int> ticks{0};
    std::atomic<int> before_start_ticks{0};
    orbit::server::TimerHandle early;
    Server server([&](orbit::server::App& app) {
        // Added before listen(): counts from the start.
        early = app.run_every(20ms, [&] { ++before_start_ticks; });
    });
    auto handle = server.app->run_every(20ms, [&] { ++ticks; });
    EXPECT_TRUE(handle.active());
    ASSERT_TRUE(eventually([&] { return ticks.load() >= 3; }));
    EXPECT_TRUE(eventually([&] { return before_start_ticks.load() >= 3; }));

    handle.cancel();
    EXPECT_FALSE(handle.active());
    std::this_thread::sleep_for(60ms); // a run already dispatched may finish
    const int after_cancel = ticks.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(ticks.load(), after_cancel);

    server.stop();
    const int at_stop = before_start_ticks.load();
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(before_start_ticks.load(), at_stop) << "timers stop with the server";
}

TEST(LifecycleTest, RunAfterRunsOnce) {
    std::atomic<int> runs{0};
    Server server([](orbit::server::App&) {});
    auto handle = server.app->run_after(30ms, [&] { ++runs; });
    ASSERT_TRUE(eventually([&] { return runs.load() == 1; }));
    EXPECT_TRUE(eventually([&] { return !handle.active(); }));
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(runs, 1);
}

// A slow callback does not pile up runs, and one that throws keeps ticking.
TEST(LifecycleTest, RunsNeverOverlapAndExceptionsAreContained) {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<int> slow_runs{0};
    std::atomic<int> throwing_runs{0};
    Server server([](orbit::server::App&) {});
    server.app->run_every(10ms, [&] {
        int now = ++active;
        int seen = max_active.load();
        while (now > seen && !max_active.compare_exchange_weak(seen, now)) {}
        std::this_thread::sleep_for(50ms);
        --active;
        ++slow_runs;
    });
    server.app->run_every(10ms, [&] {
        ++throwing_runs;
        throw std::runtime_error("timer failed");
    });
    ASSERT_TRUE(eventually([&] { return slow_runs.load() >= 3 && throwing_runs.load() >= 3; }));
    EXPECT_EQ(max_active.load(), 1);
}

TEST(LifecycleTest, NonPositiveIntervalIsRejected) {
    orbit::server::App app(orbit::test::server_config());
    EXPECT_THROW(app.run_every(0ms, [] {}), std::invalid_argument);
}
