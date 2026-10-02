#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kPort = 8095;

network::socket_t connect_client() {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_all(network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

// Reads until `needle` shows up, the peer closes, or the timeout expires.
std::string read_until(network::socket_t fd, const std::string& needle) {
    std::string out;
    char buf[1024];
    while (out.find(needle) == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

std::string masked_text_frame(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    f.push_back(static_cast<char>(0x80 | payload.size()));
    f.append(4, '\0'); // zero mask key keeps the payload unchanged
    f += payload;
    return f;
}

const char* kHandshake =
    "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

} // namespace

class WebSocketUpgradeE2ETest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg;
        cfg.port = kPort;
        app = new server::App(cfg);
        app->ws("/ws", [](http::websocket::WebSocketConnection& ws) {
            ws.on_message([&ws](const std::string& msg) { ws.send("echo:" + msg); });
        });
        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        network::socket_t wake = connect_client();
        send_all(wake, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
        read_until(wake, "\r\n\r\n");
        network::close_socket(wake);
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

server::App* WebSocketUpgradeE2ETest::app = nullptr;
std::thread WebSocketUpgradeE2ETest::server_thread;

TEST_F(WebSocketUpgradeE2ETest, ReadsFramesSentAfterTheHandshake) {
    network::socket_t fd = connect_client();
    send_all(fd, kHandshake);
    ASSERT_NE(read_until(fd, "\r\n\r\n").find("101 Switching Protocols"), std::string::npos);

    send_all(fd, masked_text_frame("one"));
    EXPECT_NE(read_until(fd, "echo:one").find("echo:one"), std::string::npos);

    send_all(fd, masked_text_frame("two"));
    EXPECT_NE(read_until(fd, "echo:two").find("echo:two"), std::string::npos);
    network::close_socket(fd);
}

TEST_F(WebSocketUpgradeE2ETest, StillHandlesFramePipelinedWithHandshake) {
    network::socket_t fd = connect_client();
    send_all(fd, std::string(kHandshake) + masked_text_frame("early"));
    EXPECT_NE(read_until(fd, "echo:early").find("echo:early"), std::string::npos);
    network::close_socket(fd);
}
