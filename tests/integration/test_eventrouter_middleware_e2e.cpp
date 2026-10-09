#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/websocket/EventRouter.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include "../utils/TestConfig.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

// EventRouter::attach() takes handshake middleware, and on_connect can see
// the handshake request (#193). An EventRouter endpoint used to accept a
// WebSocket from any Origin, with no way to authenticate it.

using namespace orbit::http;

namespace {

constexpr uint16_t kPort = 8176;

struct UserSession {
    std::string user_id;
};

orbit::network::socket_t open_socket() {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        orbit::network::close_socket(fd);
        return orbit::network::INVALID_SOCKET_FD;
    }
#ifdef _WIN32
    DWORD timeout_ms = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

// Opens the WebSocket and returns the handshake response followed by the
// first frame's payload (if the upgrade succeeded), as "<status line>|<payload>".
std::string handshake(const std::string& origin, const std::string& token) {
    orbit::network::socket_t fd = open_socket();
    if (fd == orbit::network::INVALID_SOCKET_FD) return "connect failed";
    std::string req = "GET /events HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    if (!origin.empty()) req += "Origin: " + origin + "\r\n";
    if (!token.empty()) req += "X-Token: " + token + "\r\n";
    req += "\r\n";
    ::send(fd, req.data(), static_cast<int>(req.size()), 0);

    std::string got;
    char buf[2048];
    size_t head_end = std::string::npos;
    while ((head_end = got.find("\r\n\r\n")) == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    std::string status = got.substr(0, got.find("\r\n"));
    std::string payload;
    if (status.find(" 101 ") != std::string::npos && head_end != std::string::npos) {
        std::string frame = got.substr(head_end + 4);
        // One small unmasked text frame: 0x81, length (< 126), payload.
        while (frame.size() < 2 || frame.size() < 2 + static_cast<unsigned char>(frame[1])) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            frame.append(buf, static_cast<size_t>(n));
        }
        if (frame.size() >= 2) payload = frame.substr(2, static_cast<unsigned char>(frame[1]));
    }
    orbit::network::close_socket(fd);
    return status + "|" + payload;
}

} // namespace

class EventRouterMiddlewareTest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::unique_ptr<orbit::websocket::EventRouter<UserSession>> events;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<orbit::server::App>(cfg);
        app->get("/ping", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { w->send(HttpResponse().send("ok")); });

        events = std::make_unique<orbit::websocket::EventRouter<UserSession>>();
        // Stands in for jwt_auth(): authenticates the handshake and sets req.user.
        orbit::routing::Middleware authenticate = [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            auto token = req.headers.find("X-Token");
            if (token == req.headers.end() || token->second != "secret") {
                w->send(HttpResponse().status(HttpStatus::Unauthorized).send("no"));
                return false;
            }
            req.user = {{"id", "user-42"}};
            return true;
        };
        events->on_connect([](orbit::websocket::EventSocket<UserSession>& socket, const HttpRequest& req) {
            socket.session().user_id = req.user.value("id", "");
            socket.emit("welcome", socket.session().user_id);
        });
        events->attach(*app, "/events",
                       {orbit::middleware::require_origin({"https://app.example"}), authenticate});

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            orbit::network::socket_t fd = open_socket();
            if (fd != orbit::network::INVALID_SOCKET_FD) {
                orbit::network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
        events.reset();
    }
};

std::unique_ptr<orbit::server::App> EventRouterMiddlewareTest::app;
std::unique_ptr<orbit::websocket::EventRouter<UserSession>> EventRouterMiddlewareTest::events;
std::thread EventRouterMiddlewareTest::server_thread;

TEST_F(EventRouterMiddlewareTest, ForeignOriginIsRefused) {
    std::string r = handshake("https://evil.example", "secret");
    EXPECT_NE(r.find(" 403 "), std::string::npos) << r;
}

TEST_F(EventRouterMiddlewareTest, UnauthenticatedHandshakeIsRefused) {
    std::string r = handshake("https://app.example", "");
    EXPECT_NE(r.find(" 401 "), std::string::npos) << r;
}

// on_connect initialises the session from what the middleware established.
TEST_F(EventRouterMiddlewareTest, OnConnectSeesTheAuthenticatedRequest) {
    std::string r = handshake("https://app.example", "secret");
    ASSERT_NE(r.find(" 101 "), std::string::npos) << r;
    EXPECT_NE(r.find(R"({"data":"user-42","event":"welcome"})"), std::string::npos) << r;
}
