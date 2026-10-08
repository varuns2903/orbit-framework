#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kPort = 8094;

struct RawClient {
    orbit::network::socket_t fd{orbit::network::INVALID_SOCKET_FD};

    RawClient() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
        DWORD timeout_ms = 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    ~RawClient() { orbit::network::close_socket(fd); }

    void send_bytes(const std::string& data) {
        ::send(fd, data.data(), static_cast<int>(data.size()), 0);
    }

    // Reads until the peer closes or goes quiet.
    std::string read_all() {
        std::string out;
        char buf[4096];
        while (true) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    std::string handshake() {
        send_bytes("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        std::string out;
        char buf[1024];
        while (out.find("\r\n\r\n") == std::string::npos) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }
};

// Masked client frame (mask key 0 keeps the payload readable).
std::string masked_frame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    f.push_back(static_cast<char>(0x80 | payload.size()));
    f.append(4, '\0');
    f += payload;
    return f;
}

bool has_close_code(const std::string& data, uint16_t code) {
    std::string expected;
    expected.push_back(static_cast<char>(0x88));
    expected.push_back(0x02);
    expected.push_back(static_cast<char>(code >> 8));
    expected.push_back(static_cast<char>(code & 0xFF));
    return data.find(expected) != std::string::npos;
}

std::atomic<int> g_messages{0};

} // namespace

class WebSocketLimitsE2ETest : public ::testing::Test {
protected:
    static orbit::server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new orbit::server::App(cfg);
        app->ws("/ws", [](orbit::http::websocket::WebSocketConnection& ws) {
            ws.set_max_message_size(1024);
            ws.on_message([&ws](const std::string& msg) {
                ++g_messages;
                ws.send("echo:" + msg);
            });
        });
        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        { RawClient wake; wake.send_bytes("GET / HTTP/1.1\r\nConnection: close\r\n\r\n"); wake.read_all(); }
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

orbit::server::App* WebSocketLimitsE2ETest::app = nullptr;
std::thread WebSocketLimitsE2ETest::server_thread;

TEST_F(WebSocketLimitsE2ETest, EchoStillWorks) {
    RawClient c;
    ASSERT_NE(c.handshake().find("101"), std::string::npos);
    c.send_bytes(masked_frame(0x1, "hi"));
    std::string reply = c.read_all();
    EXPECT_NE(reply.find("echo:hi"), std::string::npos);
}

TEST_F(WebSocketLimitsE2ETest, WrappingLengthFailsWithProtocolError) {
    RawClient c;
    ASSERT_NE(c.handshake().find("101"), std::string::npos);
    std::string frame = "\x82\xFF";
    frame += std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xF8", 8);
    frame += std::string(4, '\0');
    frame += "abcdef";
    c.send_bytes(frame);
    EXPECT_TRUE(has_close_code(c.read_all(), 1002));

    RawClient after;
    EXPECT_NE(after.handshake().find("101"), std::string::npos) << "server stopped accepting after bad frame";
}

TEST_F(WebSocketLimitsE2ETest, OversizedFrameIsRejectedFromHeaderAlone) {
    RawClient c;
    ASSERT_NE(c.handshake().find("101"), std::string::npos);
    int before = g_messages.load();
    // Announce 1 MiB without sending it; the limit is 1 KiB.
    std::string frame = "\x82\xFF";
    frame += std::string("\x00\x00\x00\x00\x00\x10\x00\x00", 8);
    frame += std::string(4, '\0');
    c.send_bytes(frame);
    EXPECT_TRUE(has_close_code(c.read_all(), 1009));
    EXPECT_EQ(g_messages.load(), before);
}

TEST_F(WebSocketLimitsE2ETest, OversizedControlFrameIsRejected) {
    RawClient c;
    ASSERT_NE(c.handshake().find("101"), std::string::npos);
    std::string frame = "\x89\xFE";
    frame += std::string("\x00\x80", 2);
    frame += std::string(4, '\0');
    frame += std::string(128, 'p');
    c.send_bytes(frame);
    EXPECT_TRUE(has_close_code(c.read_all(), 1002));
}
