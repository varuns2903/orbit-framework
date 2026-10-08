#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kPort = 8098;

orbit::network::socket_t connect_client() {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

// Sends a request and returns the response head (status line + headers).
std::string exchange(const std::string& request) {
    orbit::network::socket_t fd = connect_client();
    ::send(fd, request.data(), static_cast<int>(request.size()), 0);
    std::string out;
    char buf[1024];
    while (out.find("\r\n\r\n") == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    orbit::network::close_socket(fd);
    return out;
}

std::string handshake(const std::string& path, const std::string& extra_headers = "",
                      const std::string& method = "GET", const std::string& upgrade = "websocket",
                      const std::string& version = "13", const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: " + upgrade +
           "\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: " + key +
           "\r\nSec-WebSocket-Version: " + version + "\r\n" + extra_headers + "\r\n";
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

std::atomic<int> g_opened{0};

orbit::routing::Middleware require_token() {
    return [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> w) {
        auto it = req.headers.find("X-Token");
        if (it != req.headers.end() && it->second == "let-me-in") return true;
        orbit::http::HttpResponse res;
        res.status(orbit::http::HttpStatus::Unauthorized).send("401 Unauthorized");
        w->send(std::move(res));
        return false;
    };
}

void on_open(orbit::http::websocket::WebSocketConnection&) { ++g_opened; }

} // namespace

class WebSocketHandshakeTest : public ::testing::Test {
protected:
    static orbit::server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new orbit::server::App(cfg);

        app->ws("/open", on_open);
        app->ws("/route-guarded", {require_token()}, on_open);
        app->ws("/origin-guarded", {orbit::middleware::require_origin({"https://good.example"})}, on_open);
        app->group("/group", [](orbit::routing::Router& r) {
            r.use(require_token());
            r.ws("/ws", on_open);
        });
        app->get("/ping", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            res.set_body("pong");
            w->send(std::move(res));
        });

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        exchange("GET /ping HTTP/1.1\r\nConnection: close\r\n\r\n");
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

orbit::server::App* WebSocketHandshakeTest::app = nullptr;
std::thread WebSocketHandshakeTest::server_thread;

TEST_F(WebSocketHandshakeTest, OpenRouteUpgrades) {
    EXPECT_TRUE(starts_with(exchange(handshake("/open")), "HTTP/1.1 101"));
}

TEST_F(WebSocketHandshakeTest, UpgradeTokenIsCaseInsensitive) {
    EXPECT_TRUE(starts_with(exchange(handshake("/open", "", "GET", "WebSocket")), "HTTP/1.1 101"));
}

TEST_F(WebSocketHandshakeTest, RouteMiddlewareGuardsUpgrade) {
    int before = g_opened.load();
    EXPECT_TRUE(starts_with(exchange(handshake("/route-guarded")), "HTTP/1.1 401"));
    EXPECT_EQ(g_opened.load(), before);
    EXPECT_TRUE(starts_with(exchange(handshake("/route-guarded", "X-Token: let-me-in\r\n")), "HTTP/1.1 101"));
}

TEST_F(WebSocketHandshakeTest, GroupMiddlewareGuardsUpgrade) {
    EXPECT_TRUE(starts_with(exchange(handshake("/group/ws")), "HTTP/1.1 401"));
    EXPECT_TRUE(starts_with(exchange(handshake("/group/ws", "X-Token: let-me-in\r\n")), "HTTP/1.1 101"));
}

TEST_F(WebSocketHandshakeTest, RequireOriginBlocksCrossSiteHandshake) {
    EXPECT_TRUE(starts_with(exchange(handshake("/origin-guarded", "Origin: https://evil.example\r\n")), "HTTP/1.1 403"));
    EXPECT_TRUE(starts_with(exchange(handshake("/origin-guarded", "Origin: https://good.example\r\n")), "HTTP/1.1 101"));
    EXPECT_TRUE(starts_with(exchange(handshake("/origin-guarded")), "HTTP/1.1 101")); // non-browser client
}

TEST_F(WebSocketHandshakeTest, RejectsNonGetHandshake) {
    EXPECT_TRUE(starts_with(exchange(handshake("/open", "", "POST")), "HTTP/1.1 400"));
}

TEST_F(WebSocketHandshakeTest, RejectsUnsupportedVersion) {
    std::string res = exchange(handshake("/open", "", "GET", "websocket", "8"));
    EXPECT_TRUE(starts_with(res, "HTTP/1.1 400")) << res;
    EXPECT_NE(res.find("Sec-WebSocket-Version: 13"), std::string::npos) << res;
}

TEST_F(WebSocketHandshakeTest, RejectsMalformedKey) {
    EXPECT_TRUE(starts_with(exchange(handshake("/open", "", "GET", "websocket", "13", "short")), "HTTP/1.1 400"));
}

TEST_F(WebSocketHandshakeTest, RejectsMissingConnectionUpgrade) {
    std::string req = "GET /open HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    EXPECT_TRUE(starts_with(exchange(req), "HTTP/1.1 400"));
}
