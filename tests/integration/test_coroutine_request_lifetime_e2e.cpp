#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include "../utils/TestConfig.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A coroutine handler's HttpRequest& stays valid until the coroutine ends
// (#200): across a co_await that resumes on another thread, and after the
// response has gone out and the HTTP/1.1 connection has parsed the next
// request. That used to reset the request in place, so a handler that
// answered first and then read the request (an audit log, say) read freed
// or foreign data.

using namespace orbit::http;
using orbit::concurrency::Task;

namespace {

constexpr uint16_t kPort = 8174;

orbit::concurrency::ThreadPool* g_pool = nullptr;

struct ResumeOnPool {
    std::chrono::milliseconds delay;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        g_pool->enqueue([h, d = delay] {
            std::this_thread::sleep_for(d);
            h.resume();
        });
    }
    void await_resume() const noexcept {}
};

// What each handler read from its request after its co_awaits.
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<std::string> g_seen;

void record(std::string s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_seen.push_back(std::move(s));
    g_cv.notify_all();
}

std::string describe(const HttpRequest& req) {
    auto tag = req.headers.find("X-Tag");
    return req.uri + " " + (tag == req.headers.end() ? "<no tag>" : std::string(tag->second)) + " " +
           std::string(req.body);
}

// Sends @p raw on one connection and reads until @p responses complete
// responses have arrived (each ends with its "ok" body), or 3 s pass.
std::string exchange(const std::string& raw, size_t responses) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        orbit::network::close_socket(fd);
        return "";
    }
#ifdef _WIN32
    DWORD timeout_ms = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    ::send(fd, raw.data(), static_cast<int>(raw.size()), 0);
    std::string out;
    char buf[4096];
    auto count = [&out] {
        size_t n = 0;
        for (size_t at = out.find("HTTP/1.1 "); at != std::string::npos; at = out.find("HTTP/1.1 ", at + 1)) ++n;
        return n;
    };
    while (count() < responses || out.size() < 2 || out.compare(out.size() - 2, 2, "ok") != 0) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    orbit::network::close_socket(fd);
    return out;
}

std::vector<std::string> wait_for_seen(size_t n) {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait_for(lock, std::chrono::seconds(5), [n] { return g_seen.size() >= n; });
    return g_seen;
}

std::string post(const std::string& path, const std::string& tag, const std::string& body) {
    return "POST " + path + " HTTP/1.1\r\nHost: x\r\nX-Tag: " + tag + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

} // namespace

class CoroutineRequestLifetimeTest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<orbit::server::App>(cfg);

        app->get("/ping", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            w->send(HttpResponse().send("ok"));
        });
        // Reads the request after resuming on another thread.
        app->post("/read-later", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) -> Task {
            co_await ResumeOnPool{std::chrono::milliseconds(20)};
            record(describe(req));
            w->send(HttpResponse().send("ok"));
        });
        // Answers first, then reads the request once the connection has
        // moved on to the next one.
        app->post("/answer-first", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) -> Task {
            w->send(HttpResponse().send("ok"));
            co_await ResumeOnPool{std::chrono::milliseconds(150)};
            record(describe(req));
        });

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100 && exchange("GET /ping HTTP/1.1\r\nHost: x\r\n\r\n", 1).find("ok") == std::string::npos; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        g_pool = &app->get_thread_pool();
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
    }

    void SetUp() override {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_seen.clear();
    }
};

std::unique_ptr<orbit::server::App> CoroutineRequestLifetimeTest::app;
std::thread CoroutineRequestLifetimeTest::server_thread;

TEST_F(CoroutineRequestLifetimeTest, RequestSurvivesACoAwaitOnAnotherThread) {
    std::string out = exchange(post("/read-later", "first", "body-one"), 1);
    ASSERT_NE(out.find("200 OK"), std::string::npos) << out;
    auto seen = wait_for_seen(1);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "/read-later first body-one");
}

// Two pipelined requests on one keep-alive connection: the second is parsed
// while the first handler is still suspended after answering.
TEST_F(CoroutineRequestLifetimeTest, RequestSurvivesTheResponseAndTheNextRequest) {
    std::string out = exchange(post("/answer-first", "alpha", "first-body") +
                               post("/answer-first", "beta", "second-body-is-longer"), 2);
    ASSERT_NE(out.find("200 OK"), std::string::npos) << out;
    auto seen = wait_for_seen(2);
    ASSERT_EQ(seen.size(), 2u);
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen[0], "/answer-first alpha first-body");
    EXPECT_EQ(seen[1], "/answer-first beta second-body-is-longer");
}
