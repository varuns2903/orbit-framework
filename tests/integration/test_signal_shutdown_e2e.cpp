#include <gtest/gtest.h>

#ifndef _WIN32
#include <orbit/server/App.hpp>
#include <orbit/utils/Logger.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <pthread.h>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

// Starts an App on a detached thread and reports when listen() returns.
// Detached so that a hang fails the test instead of hanging the binary.
struct RunningApp {
    server::App* app;
    std::shared_ptr<std::atomic<bool>> returned = std::make_shared<std::atomic<bool>>(false);

    explicit RunningApp(uint16_t port) {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = port;
        app = new server::App(cfg);
        app->get("/", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });
        auto done = returned;
        server::App* a = app;
        std::thread([a, done] {
            a->listen();
            *done = true;
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    bool wait_for_exit(std::chrono::milliseconds limit) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!*returned && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return *returned;
    }
};

} // namespace

TEST(SignalShutdownTest, SigtermOnAnotherThreadStopsIdleServer) {
    RunningApp running(8106);
    // raise() delivers to this thread, not the event loop, which is idle in
    // its poll call. The loop must still notice the request.
    raise(SIGTERM);
    EXPECT_TRUE(running.wait_for_exit(std::chrono::seconds(3)));
    if (*running.returned) delete running.app; // otherwise leaked: the loop thread may still use it
}

TEST(SignalShutdownTest, SignalWhileThreadHoldsLoggerDoesNotDeadlock) {
    RunningApp running(8107);
    std::atomic<bool> keep_logging{true};
    std::atomic<long> lines{0};
    std::thread logger([&] {
        while (keep_logging) {
            LOG_ERROR("signal-safety test line " << lines++);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Interrupt the thread that is (almost always) inside the logger's lock.
    pthread_kill(logger.native_handle(), SIGINT);

    // Generous: the loop logs its shutdown through the same mutex the
    // logging thread hammers, and under Valgrind (one thread at a time) that
    // took 3.3 s. A real deadlock never finishes, so a long limit still
    // catches it.
    bool stopped = running.wait_for_exit(std::chrono::seconds(10));
    long before = lines.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool logger_alive = lines.load() > before;
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(logger_alive) << "logging thread is stuck inside the signal handler";

    keep_logging = false;
    if (logger_alive) {
        logger.join();
    } else {
        logger.detach();
    }
    if (stopped) delete running.app;
}
#endif // _WIN32
