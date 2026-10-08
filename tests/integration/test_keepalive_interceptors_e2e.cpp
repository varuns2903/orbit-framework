#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kPort = 8097;

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

void send_all(orbit::network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

std::string read_until(orbit::network::socket_t fd, const std::string& needle) {
    std::string out;
    char buf[1024];
    while (out.find(needle) == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

class KeepAliveInterceptorTest : public ::testing::Test {
protected:
    static orbit::server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new orbit::server::App(cfg);

        // Counts how many interceptors touch each response.
        app->use([](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            w->add_interceptor([](orbit::http::HttpResponse& res) {
                auto it = res.headers.find("X-Hits");
                int hits = it == res.headers.end() ? 0 : std::stoi(it->second);
                res.headers["X-Hits"] = std::to_string(hits + 1);
            });
            return true;
        });
        app->get("/n", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        orbit::network::socket_t wake = connect_client();
        send_all(wake, "GET /n HTTP/1.1\r\nConnection: close\r\n\r\n");
        read_until(wake, "ok");
        orbit::network::close_socket(wake);
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

orbit::server::App* KeepAliveInterceptorTest::app = nullptr;
std::thread KeepAliveInterceptorTest::server_thread;

TEST_F(KeepAliveInterceptorTest, InterceptorsApplyOnlyToTheirOwnRequest) {
    orbit::network::socket_t fd = connect_client();
    for (int i = 1; i <= 3; ++i) {
        send_all(fd, "GET /n HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string res = read_until(fd, "ok");
        EXPECT_NE(res.find("X-Hits: 1\r\n"), std::string::npos) << "request " << i << ":\n" << res;
    }
    orbit::network::close_socket(fd);
}
