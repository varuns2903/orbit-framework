#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

using namespace http;
namespace fs = std::filesystem;

namespace {

constexpr uint16_t kPort = 8111;

// Sends raw bytes and returns everything the server writes back until it
// closes the connection or goes quiet.
std::string raw_exchange(const std::string& bytes) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return "<connect failed>";
    }
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    ::send(fd, bytes.data(), static_cast<int>(bytes.size()), 0);

    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    network::close_socket(fd);
    return out;
}

std::string body_of(const std::string& response) {
    size_t end = response.find("\r\n\r\n");
    return end == std::string::npos ? "" : response.substr(end + 4);
}

} // namespace

class StaticFilesE2ETest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;
    static fs::path dir;

    static void SetUpTestSuite() {
        dir = fs::temp_directory_path() / "orbit_static_e2e";
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ofstream(dir / "alphabet.txt", std::ios::binary) << "abcdefghijklmnopqrstuvwxyz";

        config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new server::App(cfg);
        app->use(middleware::static_files(dir.string()));

        server_thread = std::thread([] { app->listen(); });
        // Wait until the listener accepts connections.
        for (int i = 0; i < 100; ++i) {
            std::string probe = raw_exchange("GET /alphabet.txt HTTP/1.1\r\nConnection: close\r\n\r\n");
            if (probe.rfind("HTTP/1.1", 0) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        // Wake the event loop so it notices the stop flag.
        raw_exchange("GET /alphabet.txt HTTP/1.1\r\nConnection: close\r\n\r\n");
        if (server_thread.joinable()) server_thread.join();
        delete app;
        fs::remove_all(dir);
    }
};

server::App* StaticFilesE2ETest::app = nullptr;
std::thread StaticFilesE2ETest::server_thread;
fs::path StaticFilesE2ETest::dir;

TEST_F(StaticFilesE2ETest, ServesWholeFile) {
    std::string res = raw_exchange("GET /alphabet.txt HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_EQ(body_of(res), "abcdefghijklmnopqrstuvwxyz");
}

TEST_F(StaticFilesE2ETest, RangeSendsOnlyThoseBytes) {
    std::string res = raw_exchange("GET /alphabet.txt HTTP/1.1\r\nRange: bytes=3-6\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 206", 0), 0u) << res;
    EXPECT_NE(res.find("Content-Range: bytes 3-6/26"), std::string::npos) << res;
    EXPECT_EQ(body_of(res), "defg");

    std::string tail = raw_exchange("GET /alphabet.txt HTTP/1.1\r\nRange: bytes=-2\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(body_of(tail), "yz");
}

TEST_F(StaticFilesE2ETest, HeadSendsHeadersOnly) {
    std::string res = raw_exchange("HEAD /alphabet.txt HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_NE(res.find("Content-Length: 26"), std::string::npos) << res;
    EXPECT_EQ(body_of(res), "");
}

TEST_F(StaticFilesE2ETest, RangeThenFullFileOnOneConnection) {
    // A ranged response must not leave the offset behind for the next one.
    std::string res = raw_exchange(
        "GET /alphabet.txt HTTP/1.1\r\nRange: bytes=24-\r\n\r\n"
        "GET /alphabet.txt HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_NE(res.find("yzHTTP/1.1 200"), std::string::npos) << res;
    EXPECT_NE(res.find("abcdefghijklmnopqrstuvwxyz"), std::string::npos) << res;
}
