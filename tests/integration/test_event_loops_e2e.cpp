#include <gtest/gtest.h>

#ifdef __linux__
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <netinet/in.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Several event loops (ServerConfig::event_loops), each with its own
// SO_REUSEPORT listening socket: connections are spread across them, every
// one is served, max_connections stays one limit, and a graceful shutdown
// drains them all.

using namespace http;

namespace {

int connect_to(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{0, 200 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// Reads until `needle` has been seen `times` times, the peer closes, or
// `limit` passes.
std::string read_until(int fd, const std::string& needle, size_t times,
                       std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    std::string data;
    auto deadline = std::chrono::steady_clock::now() + limit;
    char buf[4096];
    auto count = [&] {
        size_t n = 0;
        for (size_t p = data.find(needle); p != std::string::npos; p = data.find(needle, p + 1)) ++n;
        return n;
    };
    while (count() < times && std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) data.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
    }
    return data;
}

size_t total(const std::vector<size_t>& counts) {
    return std::accumulate(counts.begin(), counts.end(), size_t{0});
}

// An App on its own thread, with /ping and /slow (answers after `slow`).
class Server {
public:
    Server(int port, size_t loops, size_t max_connections = 0,
           std::chrono::milliseconds slow = std::chrono::milliseconds(400))
        : port_(port) {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = static_cast<uint16_t>(port);
        cfg.event_loops = loops;
        cfg.max_connections = max_connections;
        cfg.worker_threads = 8;
        app_ = std::make_unique<server::App>(cfg);
        app_->get("/ping", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("pong", "text/plain");
            w->send(std::move(res));
        });
        app_->get("/slow", [slow](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            std::this_thread::sleep_for(slow);
            HttpResponse res;
            res.set_body("done", "text/plain");
            w->send(std::move(res));
        });
        thread_ = std::thread([this] {
            app_->listen();
            returned_ = true;
        });
        for (int i = 0; i < 100; ++i) {
            int fd = connect_to(port_);
            if (fd >= 0) {
                ::close(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ~Server() {
        app_->stop();
        if (thread_.joinable()) thread_.join();
    }
    server::App& app() { return *app_; }
    bool returned_within(std::chrono::milliseconds limit) {
        auto deadline = std::chrono::steady_clock::now() + limit;
        while (!returned_ && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return returned_;
    }
    int port() const { return port_; }

private:
    int port_;
    std::unique_ptr<server::App> app_;
    std::thread thread_;
    std::atomic<bool> returned_{false};
};

} // namespace

TEST(EventLoopsTest, OneLoopByDefault) {
    Server s(8160, 1);
    int fd = connect_to(s.port());
    ASSERT_GE(fd, 0);
    send_all(fd, "GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
    EXPECT_NE(read_until(fd, "pong", 1).find("pong"), std::string::npos);
    EXPECT_EQ(s.app().connections_per_event_loop().size(), 1u);
    ::close(fd);
}

TEST(EventLoopsTest, ConnectionsAreSpreadAndAllServed) {
    constexpr int kConnections = 64;
    constexpr int kRequestsEach = 3;
    Server s(8161, 4);
    std::vector<int> fds;
    for (int i = 0; i < kConnections; ++i) {
        int fd = connect_to(s.port());
        ASSERT_GE(fd, 0) << i;
        fds.push_back(fd);
    }
    int answered = 0;
    for (int round = 0; round < kRequestsEach; ++round) {
        for (int fd : fds) send_all(fd, "GET /ping HTTP/1.1\r\nHost: h\r\n\r\n");
        for (int fd : fds) {
            if (read_until(fd, "pong", 1).find("pong") != std::string::npos) ++answered;
        }
    }
    EXPECT_EQ(answered, kConnections * kRequestsEach);

    // Every connection is still open (keep-alive) and belongs to one loop.
    auto counts = s.app().connections_per_event_loop();
    ASSERT_EQ(counts.size(), 4u);
    EXPECT_EQ(total(counts), static_cast<size_t>(kConnections));
    size_t loops_used = 0;
    for (size_t c : counts) loops_used += c > 0 ? 1 : 0;
    // The kernel hashes connections across the sockets; 64 of them on one
    // loop out of four would mean the sockets are not sharing the port.
    EXPECT_GE(loops_used, 2u) << counts[0] << " " << counts[1] << " " << counts[2] << " " << counts[3];
    for (int fd : fds) ::close(fd);
}

TEST(EventLoopsTest, MaxConnectionsIsOneLimitAcrossLoops) {
    constexpr int kClients = 32;
    Server s(8162, 4, /*max_connections=*/3);
    std::vector<int> fds;
    for (int i = 0; i < kClients; ++i) {
        int fd = connect_to(s.port()); // beyond the limit these wait in the backlog
        ASSERT_GE(fd, 0);
        fds.push_back(fd);
    }
    // Four loops accept concurrently; checking the count and then accepting
    // let them overshoot together. Sample it while they compete.
    size_t most = 0;
    for (int i = 0; i < 15; ++i) {
        most = std::max(most, total(s.app().connections_per_event_loop()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_LE(most, 3u);

    // Each finished connection frees a slot for a waiting one; all get answered.
    int answered = 0;
    for (int fd : fds) {
        send_all(fd, "GET /ping HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    }
    for (int fd : fds) {
        if (read_until(fd, "pong", 1).find("pong") != std::string::npos) ++answered;
        ::close(fd);
    }
    EXPECT_EQ(answered, kClients);
    // Released slots are counted back: the budget is free again.
    for (int i = 0; i < 50 && total(s.app().connections_per_event_loop()) != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(total(s.app().connections_per_event_loop()), 0u);
}

TEST(EventLoopsTest, GracefulShutdownDrainsEveryLoop) {
    Server s(8163, 4);
    std::vector<int> fds;
    for (int i = 0; i < 16; ++i) {
        int fd = connect_to(s.port());
        ASSERT_GE(fd, 0);
        send_all(fd, "GET /slow HTTP/1.1\r\nHost: h\r\n\r\n");
        fds.push_back(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // handlers running
    s.app().shutdown(std::chrono::seconds(5));

    int finished = 0;
    for (int fd : fds) {
        if (read_until(fd, "done", 1).find("done") != std::string::npos) ++finished;
        ::close(fd);
    }
    EXPECT_EQ(finished, 16) << "an in-flight request was cut off";
    EXPECT_TRUE(s.returned_within(std::chrono::seconds(5))) << "listen() did not return after draining";
}

TEST(EventLoopsTest, StopEndsEveryLoop) {
    Server s(8164, 3);
    int fd = connect_to(s.port());
    ASSERT_GE(fd, 0);
    s.app().stop();
    EXPECT_TRUE(s.returned_within(std::chrono::seconds(5)));
    ::close(fd);
}

#endif
