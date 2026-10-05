#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

network::socket_t connect_to(uint16_t port, int timeout_ms = 3000) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return network::INVALID_SOCKET_FD;
    }
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_str(network::socket_t fd, const std::string& s) {
    ::send(fd, s.data(), static_cast<int>(s.size()), 0);
}

std::string read_some(network::socket_t fd) {
    char buf[4096];
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

std::string exchange(uint16_t port, const std::string& request) {
    network::socket_t fd = connect_to(port);
    if (fd == network::INVALID_SOCKET_FD) return "<connect failed>";
    send_str(fd, request);
    std::string out;
    for (std::string chunk = read_some(fd); !chunk.empty(); chunk = read_some(fd)) out += chunk;
    network::close_socket(fd);
    return out;
}

// Masked client frame (RFC 6455).
std::string client_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    f.push_back(static_cast<char>(0x80 | payload.size())); // payloads here are < 126
    const char key[4] = {1, 2, 3, 4};
    f.append(key, 4);
    for (size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
    return f;
}

// Opens a WebSocket on /ws; returns the socket after the 101 response.
network::socket_t open_websocket(uint16_t port, int timeout_ms) {
    network::socket_t fd = connect_to(port, timeout_ms);
    if (fd == network::INVALID_SOCKET_FD) return fd;
    send_str(fd, "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    std::string res;
    while (res.find("\r\n\r\n") == std::string::npos) {
        std::string chunk = read_some(fd);
        if (chunk.empty()) break;
        res += chunk;
    }
    EXPECT_NE(res.find("101"), std::string::npos) << res;
    return fd;
}

class Server {
public:
    explicit Server(const config::ServerConfig& cfg) : app_(std::make_unique<server::App>(cfg)), port_(cfg.port) {
        app_->get("/ok", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });
        app_->get("/moved", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res; // no body
            res.status(http::HttpStatus::Found);
            res.headers["Location"] = "/ok";
            w->send(std::move(res));
        });
        app_->ws("/ws", [](http::websocket::WebSocketConnection& ws) {
            ws.on_message([&ws](const std::string& m) { ws.send(m); });
        });
        server::App* app = app_.get();
        thread_ = std::thread([app] { app->listen(); });
        for (int i = 0; i < 200; ++i) {
            network::socket_t fd = connect_to(port_);
            if (fd != network::INVALID_SOCKET_FD) {
                network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~Server() {
        app_->stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::unique_ptr<server::App> app_;
    uint16_t port_;
    std::thread thread_;
};

} // namespace

TEST(ServerLimitsTest, RequestLineHeaderCountAndSizeAreConfigurable) {
    config::ServerConfig cfg;
    cfg.port = 8129;
    cfg.max_request_line = 100;
    cfg.max_headers = 5;
    cfg.max_header_bytes = 512;
    Server server(cfg);

    // Within the limits.
    std::string ok = exchange(8129, "GET /ok HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(ok.rfind("HTTP/1.1 200", 0), 0u) << ok;

    std::string long_line = exchange(8129, "GET /ok?" + std::string(200, 'q') + " HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(long_line.rfind("HTTP/1.1 431", 0), 0u) << long_line;

    std::string many = "GET /ok HTTP/1.1\r\n";
    for (int i = 0; i < 10; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
    std::string too_many = exchange(8129, many + "Connection: close\r\n\r\n");
    EXPECT_EQ(too_many.rfind("HTTP/1.1 431", 0), 0u) << too_many;

    std::string big = exchange(8129, "GET /ok HTTP/1.1\r\nX-Big: " + std::string(600, 'b') + "\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(big.rfind("HTTP/1.1 431", 0), 0u) << big;
}

TEST(ServerLimitsTest, ServerPingsWebSockets) {
    config::ServerConfig cfg;
    cfg.port = 8130;
    cfg.websocket_ping_interval = std::chrono::seconds(1);
    Server server(cfg);

    network::socket_t fd = open_websocket(8130, 3000);
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    std::string frame = read_some(fd); // nothing else is sent, so this is the ping
    network::close_socket(fd);
    ASSERT_GE(frame.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(frame[0]), 0x89) << "expected a ping frame";
}

TEST(ServerLimitsTest, PingsKeepLivePeersAndIdleTimeoutDropsSilentOnes) {
    config::ServerConfig cfg;
    cfg.port = 8131;
    cfg.websocket_ping_interval = std::chrono::seconds(1);
    cfg.websocket_idle_timeout = std::chrono::seconds(2);
    Server server(cfg);

    // Live peer: answers every ping with a pong.
    std::atomic<bool> live_closed{false};
    std::thread live([&] {
        network::socket_t fd = open_websocket(8131, 500);
        auto until = Clock::now() + std::chrono::milliseconds(4500);
        while (Clock::now() < until) {
            std::string data = read_some(fd);
            if (data.empty()) {
#ifdef _WIN32
                int err = WSAGetLastError();
                bool timed_out = err == WSAETIMEDOUT;
#else
                bool timed_out = errno == EAGAIN || errno == EWOULDBLOCK;
#endif
                if (!timed_out) {
                    live_closed = true;
                    break;
                }
                continue;
            }
            if (static_cast<uint8_t>(data[0]) == 0x89) send_str(fd, client_frame(0xA, ""));
        }
        network::close_socket(fd);
    });

    // Silent peer: never reads or answers; the server must close it.
    network::socket_t silent = open_websocket(8131, 6000);
    ASSERT_NE(silent, network::INVALID_SOCKET_FD);
    auto start = Clock::now();
    std::string rest;
    for (std::string chunk = read_some(silent); !chunk.empty(); chunk = read_some(silent)) rest += chunk;
    auto closed_after = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    network::close_socket(silent);

    live.join();
    EXPECT_LT(closed_after.count(), 4000) << "silent peer was not dropped by the idle timeout";
    EXPECT_GE(closed_after.count(), 1500);
    EXPECT_FALSE(live_closed) << "a peer answering pings was disconnected";
}

TEST(ServerLimitsTest, WebSocketMessageLimitFromConfig) {
    config::ServerConfig cfg;
    cfg.port = 8132;
    cfg.websocket_max_message_size = 10;
    Server server(cfg);

    network::socket_t fd = open_websocket(8132, 3000);
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, client_frame(0x1, std::string(20, 'm')));
    std::string reply = read_some(fd);
    network::close_socket(fd);
    ASSERT_GE(reply.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(reply[0]), 0x88) << "expected a close frame";
    EXPECT_EQ((static_cast<uint8_t>(reply[2]) << 8) | static_cast<uint8_t>(reply[3]), 1009);
}

TEST(ServerLimitsTest, EmptyBodyResponseKeepsTheConnectionUsable) {
    config::ServerConfig cfg;
    cfg.port = 8147;
    Server server(cfg);

    network::socket_t fd = connect_to(8147, 2000);
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    send_str(fd, "GET /moved HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string first = read_some(fd);
    EXPECT_EQ(first.rfind("HTTP/1.1 302", 0), 0u) << first;
    EXPECT_NE(first.find("Content-Length: 0\r\n"), std::string::npos) << first;

    // The client knows the 302 ended, so it can send the next request at once.
    auto started = std::chrono::steady_clock::now();
    send_str(fd, "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    std::string second = read_some(fd);
    auto took = std::chrono::steady_clock::now() - started;
    network::close_socket(fd);
    EXPECT_EQ(second.rfind("HTTP/1.1 200", 0), 0u) << second;
    EXPECT_LT(took, std::chrono::milliseconds(1000));
}
