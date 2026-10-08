#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

using Clock = std::chrono::steady_clock;

orbit::network::socket_t connect_to(uint16_t port) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        orbit::network::close_socket(fd);
        return orbit::network::INVALID_SOCKET_FD;
    }
#ifdef _WIN32
    DWORD timeout_ms = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_str(orbit::network::socket_t fd, const std::string& s) {
    ::send(fd, s.data(), static_cast<int>(s.size()), 0);
}

// Reads until the peer closes or the 5 s receive timeout fires.
std::string read_all(orbit::network::socket_t fd) {
    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// Reads one response (headers + Content-Length body) without waiting for close.
std::string read_response(orbit::network::socket_t fd) {
    std::string out;
    char buf[4096];
    while (true) {
        size_t end = out.find("\r\n\r\n");
        if (end != std::string::npos) {
            size_t cl = out.find("Content-Length: ");
            size_t len = cl == std::string::npos ? 0 : std::stoul(out.substr(cl + 16));
            if (out.size() >= end + 4 + len) return out;
        }
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return out;
        out.append(buf, static_cast<size_t>(n));
    }
}

class Server {
public:
    explicit Server(uint16_t port, std::chrono::seconds shutdown_timeout = std::chrono::seconds(30)) : port_(port) {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = port;
        cfg.shutdown_timeout = shutdown_timeout;
        app_ = std::make_unique<orbit::server::App>(cfg);
        app_->get("/fast", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            res.set_body("fast");
            w->send(std::move(res));
        });
        app_->get("/slow", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            orbit::http::HttpResponse res;
            res.set_body("slow done");
            w->send(std::move(res));
        });
        app_->get("/hang", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::seconds(4));
            orbit::http::HttpResponse res;
            res.set_body("too late");
            w->send(std::move(res));
        });
        app_->enable_health_checks();
    }

    void start() {
        auto done = done_;
        orbit::server::App* app = app_.get();
        thread_ = std::thread([app, done] {
            app->listen();
            *done = true;
        });
        for (int i = 0; i < 200; ++i) {
            orbit::network::socket_t fd = connect_to(port_);
            if (fd != orbit::network::INVALID_SOCKET_FD) {
                orbit::network::close_socket(fd);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Time until listen() returned, or -1 ms if it did not within `limit`.
    std::chrono::milliseconds wait_stopped(std::chrono::milliseconds limit, Clock::time_point since) {
        auto deadline = Clock::now() + limit;
        while (!*done_ && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!*done_) return std::chrono::milliseconds(-1);
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since);
    }

    ~Server() {
        app_->stop();
        if (thread_.joinable()) thread_.join();
    }

    orbit::server::App& app() { return *app_; }

private:
    uint16_t port_;
    std::unique_ptr<orbit::server::App> app_;
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(false);
    std::thread thread_;
};

} // namespace

TEST(GracefulShutdownTest, InFlightRequestFinishesWithConnectionClose) {
    Server s(8122);
    s.start();
    orbit::network::socket_t fd = connect_to(8122);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // handler is now sleeping

    auto t0 = Clock::now();
    s.app().shutdown();
    std::string res = read_all(fd); // the server closes after this response
    orbit::network::close_socket(fd);

    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_NE(res.find("slow done"), std::string::npos) << res;
    EXPECT_NE(res.find("Connection: close"), std::string::npos) << res;
    auto took = s.wait_stopped(std::chrono::seconds(5), t0);
    EXPECT_GE(took.count(), 0) << "listen() did not return";
    EXPECT_LT(took.count(), 3000) << "drain waited for the timeout instead of the request";
}

TEST(GracefulShutdownTest, IdleKeepAliveConnectionsAreClosedPromptly) {
    Server s(8123);
    s.start();
    orbit::network::socket_t fd = connect_to(8123);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, "GET /fast HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string first = read_response(fd);
    ASSERT_NE(first.find("fast"), std::string::npos) << first;

    auto t0 = Clock::now();
    s.app().shutdown(); // 30 s budget; an idle connection must not use it
    std::string rest = read_all(fd);
    auto closed_after = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
    orbit::network::close_socket(fd);

    EXPECT_EQ(rest, "");
    EXPECT_LT(closed_after.count(), 2000);
    EXPECT_GE(s.wait_stopped(std::chrono::seconds(3), t0).count(), 0);
}

TEST(GracefulShutdownTest, DeadlineClosesRequestsThatDoNotFinish) {
    Server s(8124, std::chrono::seconds(1));
    s.start();
    orbit::network::socket_t fd = connect_to(8124);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, "GET /hang HTTP/1.1\r\nHost: x\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto t0 = Clock::now();
    s.app().shutdown();
    auto took = s.wait_stopped(std::chrono::seconds(4), t0);
    orbit::network::close_socket(fd);
    EXPECT_GE(took.count(), 900) << "stopped before the deadline while a request was running";
    EXPECT_LT(took.count(), 2500) << "deadline not enforced";
}

TEST(GracefulShutdownTest, NewConnectionsAreRefusedWhileDraining) {
    Server s(8125);
    s.start();
    orbit::network::socket_t busy = connect_to(8125);
    ASSERT_NE(busy, orbit::network::INVALID_SOCKET_FD);
    send_str(busy, "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    s.app().shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // loop has started draining
    orbit::network::socket_t late = connect_to(8125);
    EXPECT_EQ(late, orbit::network::INVALID_SOCKET_FD) << "listener still accepting while draining";
    if (late != orbit::network::INVALID_SOCKET_FD) orbit::network::close_socket(late);

    read_all(busy);
    orbit::network::close_socket(busy);
}

TEST(GracefulShutdownTest, HealthChecksReportDraining) {
    Server s(8126);
    s.start();
    orbit::network::socket_t fd = connect_to(8126);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_NE(read_response(fd).find("200 OK"), std::string::npos);
    send_str(fd, "GET /readyz HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string ready = read_response(fd);
    EXPECT_NE(ready.find("200 OK"), std::string::npos) << ready;
    EXPECT_FALSE(s.app().is_draining());

    // A request still in transit when draining starts (headers incomplete,
    // so the connection is not idle) is answered: readiness now says 503.
    send_str(fd, "GET /readyz HTTP/1.1\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s.app().shutdown();
    EXPECT_TRUE(s.app().is_draining());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_str(fd, "Host: x\r\n\r\n");
    std::string draining = read_all(fd);
    orbit::network::close_socket(fd);
    EXPECT_NE(draining.find("503"), std::string::npos) << draining;
    EXPECT_NE(draining.find("Connection: close"), std::string::npos) << draining;
}

#ifndef _WIN32
TEST(GracefulShutdownTest, SecondSignalStopsImmediately) {
    Server s(8127, std::chrono::seconds(30));
    s.start();
    orbit::network::socket_t fd = connect_to(8127);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, "GET /hang HTTP/1.1\r\nHost: x\r\n\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    auto t0 = Clock::now();
    std::raise(SIGTERM); // graceful: would wait for /hang (4 s)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_TRUE(s.app().is_draining());
    std::raise(SIGTERM); // second: stop now
    auto took = s.wait_stopped(std::chrono::seconds(3), t0);
    orbit::network::close_socket(fd);
    EXPECT_GE(took.count(), 0);
    EXPECT_LT(took.count(), 2000);
}
#endif
