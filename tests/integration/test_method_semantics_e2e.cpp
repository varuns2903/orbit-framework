#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kPort = 8109;

std::string exchange(const std::string& request, const std::string& until) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
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
    ::send(fd, request.data(), static_cast<int>(request.size()), 0);
    std::string out;
    char buf[2048];
    while (out.find(until) == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    network::close_socket(fd);
    return out;
}

} // namespace

class MethodSemanticsTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new server::App(cfg);
        app->get("/thing", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("hello");
            w->send(std::move(res));
        });
        app->post("/thing", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("posted");
            w->send(std::move(res));
        });
        app->get("/users/:id", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("user " + req.params["id"]);
            w->send(std::move(res));
        });
        app->del("/empty", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.status(http::HttpStatus::NoContent);
            res.body = "should not be sent";
            w->send(std::move(res));
        });
        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        exchange("GET /thing HTTP/1.1\r\nConnection: close\r\n\r\n", "hello");
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

server::App* MethodSemanticsTest::app = nullptr;
std::thread MethodSemanticsTest::server_thread;

TEST_F(MethodSemanticsTest, HeadUsesGetHandlerWithoutBody) {
    // A GET follows on the same connection: if the HEAD response carried a
    // body, it would be read as the start of the next response.
    std::string res = exchange("HEAD /thing HTTP/1.1\r\nHost: x\r\n\r\n"
                               "GET /users/7 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "user 7");
    EXPECT_EQ(res.rfind("HTTP/1.1 200 OK\r\n", 0), 0u) << res;
    EXPECT_NE(res.find("Content-Length: 5\r\n"), std::string::npos) << res;
    EXPECT_EQ(res.find("hello"), std::string::npos) << res;
    size_t second = res.find("HTTP/1.1 200", 5);
    ASSERT_NE(second, std::string::npos) << res;
    EXPECT_EQ(res.find("\r\n\r\n") + 4, second) << "HEAD response carried a body:\n" << res;
    EXPECT_NE(res.find("user 7"), std::string::npos);
}

TEST_F(MethodSemanticsTest, HeadOnDynamicRoute) {
    std::string res = exchange("HEAD /users/9 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
}

TEST_F(MethodSemanticsTest, WrongMethodIs405WithAllow) {
    std::string res = exchange("PUT /thing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 405 Method Not Allowed\r\n", 0), 0u) << res;
    EXPECT_NE(res.find("Allow: GET, HEAD, POST\r\n"), std::string::npos) << res;
}

TEST_F(MethodSemanticsTest, UnknownPathIs404) {
    std::string res = exchange("GET /missing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 404", 0), 0u) << res;
}

TEST_F(MethodSemanticsTest, NoContentResponseHasNoBody) {
    std::string res = exchange("DELETE /empty HTTP/1.1\r\nHost: x\r\n\r\n"
                               "GET /thing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "hello");
    EXPECT_EQ(res.rfind("HTTP/1.1 204 No Content\r\n", 0), 0u) << res;
    EXPECT_EQ(res.find("should not be sent"), std::string::npos) << res;
    EXPECT_NE(res.find("hello"), std::string::npos) << res;
}
