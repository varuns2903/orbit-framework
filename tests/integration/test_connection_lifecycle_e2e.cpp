#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/websocket/EventRouter.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kPort = 8096;
using Clock = std::chrono::steady_clock;

network::socket_t connect_client(int recv_timeout_ms = 3000) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(recv_timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{recv_timeout_ms / 1000, (recv_timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_all(network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

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

// True once the server has closed the connection (recv returns 0).
bool peer_closed(network::socket_t fd) {
    char buf[256];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) return true;
        if (n < 0) return false; // timed out while still open
    }
}

std::string masked_text_frame(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    f.push_back(static_cast<char>(0x80 | payload.size()));
    f.append(4, '\0');
    f += payload;
    return f;
}

std::string ws_handshake(const std::string& path) {
    return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
}

std::atomic<int> g_plain_closes{0};

} // namespace

class ConnectionLifecycleTest : public ::testing::Test {
protected:
    static server::App* app;
    static websocket::EventRouter<>* events;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.header_timeout = std::chrono::seconds(1);
        cfg.keep_alive_timeout = std::chrono::seconds(1);
        cfg.idle_timeout = std::chrono::seconds(1);
        cfg.websocket_idle_timeout = std::chrono::seconds(0);
        app = new server::App(cfg);

        app->get("/slow", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            http::HttpResponse res;
            res.set_body("done");
            w->send(std::move(res));
        });
        app->get("/fast", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("fast");
            w->send(std::move(res));
        });
        app->ws("/plain", [](http::websocket::WebSocketConnection& ws) {
            ws.on_message([&ws](const std::string& msg) { ws.send("echo:" + msg); });
            ws.on_close([] { ++g_plain_closes; });
        });

        events = new websocket::EventRouter<>();
        events->on_connect([](websocket::EventSocket<websocket::EmptySession>& s) { s.join("lobby"); });
        events->on("say", [](websocket::EventSocket<websocket::EmptySession>& s) {
            events->to("lobby").emit("said", s.id());
        });
        events->attach(*app, "/events");

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        network::socket_t wake = connect_client();
        send_all(wake, "GET /fast HTTP/1.1\r\nConnection: close\r\n\r\n");
        read_until(wake, "fast");
        network::close_socket(wake);
        if (server_thread.joinable()) server_thread.join();
        delete app;
        delete events;
    }
};

server::App* ConnectionLifecycleTest::app = nullptr;
websocket::EventRouter<>* ConnectionLifecycleTest::events = nullptr;
std::thread ConnectionLifecycleTest::server_thread;

TEST_F(ConnectionLifecycleTest, DroppedWebSocketFiresOnClose) {
    int before = g_plain_closes.load();
    network::socket_t fd = connect_client();
    send_all(fd, ws_handshake("/plain"));
    ASSERT_NE(read_until(fd, "\r\n\r\n").find("101"), std::string::npos);
    network::close_socket(fd); // no close frame

    auto deadline = Clock::now() + std::chrono::seconds(3);
    while (g_plain_closes.load() == before && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(g_plain_closes.load(), before + 1);
}

TEST_F(ConnectionLifecycleTest, BroadcastAfterPeerDropIsSafe) {
    // Under ASan this used to report a heap-use-after-free: the dropped
    // socket stayed in the room with a dangling connection reference.
    network::socket_t a = connect_client();
    network::socket_t b = connect_client();
    send_all(a, ws_handshake("/events"));
    send_all(b, ws_handshake("/events"));
    ASSERT_NE(read_until(a, "\r\n\r\n").find("101"), std::string::npos);
    ASSERT_NE(read_until(b, "\r\n\r\n").find("101"), std::string::npos);

    network::close_socket(b);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    send_all(a, masked_text_frame(R"({"event":"say"})"));
    EXPECT_NE(read_until(a, "said").find("said"), std::string::npos);
    network::close_socket(a);
}

TEST_F(ConnectionLifecycleTest, IdleWebSocketIsNotReaped) {
    network::socket_t fd = connect_client();
    send_all(fd, ws_handshake("/plain"));
    ASSERT_NE(read_until(fd, "\r\n\r\n").find("101"), std::string::npos);

    std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // > every HTTP timeout
    send_all(fd, masked_text_frame("still-here"));
    EXPECT_NE(read_until(fd, "echo:still-here").find("echo:still-here"), std::string::npos);
    network::close_socket(fd);
}

TEST_F(ConnectionLifecycleTest, SlowHandlerIsNotCutOff) {
    network::socket_t fd = connect_client(5000);
    send_all(fd, "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_NE(read_until(fd, "done").find("done"), std::string::npos);
    network::close_socket(fd);
}

TEST_F(ConnectionLifecycleTest, TricklingHeadersDoesNotExtendTheDeadline) {
    network::socket_t fd = connect_client(200);
    auto start = Clock::now();
    send_all(fd, "GET /fast HTTP/1.1\r\n");
    bool closed = false;
    for (int i = 0; i < 20 && !closed; ++i) {
        send_all(fd, "X-Drip: 1\r\n");
        closed = peer_closed(fd); // waits up to 200 ms
    }
    auto elapsed = Clock::now() - start;
    EXPECT_TRUE(closed);
    EXPECT_LT(elapsed, std::chrono::milliseconds(2500));
    network::close_socket(fd);
}

TEST_F(ConnectionLifecycleTest, IdleKeepAliveConnectionIsClosed) {
    network::socket_t fd = connect_client(3000);
    send_all(fd, "GET /fast HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_NE(read_until(fd, "fast").find("fast"), std::string::npos);
    auto start = Clock::now();
    EXPECT_TRUE(peer_closed(fd));
    EXPECT_LT(Clock::now() - start, std::chrono::milliseconds(2500));
    network::close_socket(fd);
}
