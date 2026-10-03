#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <string>
#include <thread>

using namespace http;

namespace {

constexpr uint16_t kPort = 8093;

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
    DWORD timeout_ms = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{1, 0};
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

size_t count(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) ++n;
    return n;
}

} // namespace

class Http1FramingTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.max_body_size = 64;
        app = new server::App(cfg);

        app->get("/hello", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("hello");
            w->send(std::move(res));
        });
        app->post("/echo", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("[" + std::string(req.body) + "]");
            w->send(std::move(res));
        });

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        app->stop();
        // Wake the event loop so it notices the stop flag.
        raw_exchange("GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

server::App* Http1FramingTest::app = nullptr;
std::thread Http1FramingTest::server_thread;

TEST_F(Http1FramingTest, ConflictingContentLengthIsRejectedAndServerSurvives) {
    std::string res = raw_exchange(
        "POST /echo HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: abc\r\n\r\nA");
    EXPECT_EQ(res.rfind("HTTP/1.1 400", 0), 0u) << res;

    std::string after = raw_exchange("GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_NE(after.find("hello"), std::string::npos) << after;
}

TEST_F(Http1FramingTest, TransferEncodingWithContentLengthIsRejected) {
    std::string res = raw_exchange(
        "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 400", 0), 0u) << res;
}

TEST_F(Http1FramingTest, ChunkedBodyIsDecoded) {
    std::string res = raw_exchange(
        "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n");
    EXPECT_NE(res.find("[abcde]"), std::string::npos) << res;
}

TEST_F(Http1FramingTest, OversizedChunkedBodyIsRejected) {
    std::string chunk(100, 'x');
    std::string res = raw_exchange(
        "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n64\r\n" + chunk + "\r\n0\r\n\r\n");
    EXPECT_EQ(res.rfind("HTTP/1.1 413", 0), 0u) << res;
}

TEST_F(Http1FramingTest, PipelinedRequestsAreNotMergedIntoTheBody) {
    std::string res = raw_exchange(
        "POST /echo HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc"
        "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
    EXPECT_NE(res.find("[abc]"), std::string::npos) << res;
    EXPECT_EQ(count(res, "HTTP/1.1 200"), 2u) << res;
}

TEST_F(Http1FramingTest, PipelinedResponsesAreNeverStranded) {
    // The second response is queued from a worker thread while the first is
    // still being written. A lost wakeup used to leave it unsent, so repeat
    // enough times to hit the window.
    for (int i = 0; i < 50; ++i) {
        std::string res = raw_exchange(
            "GET /hello HTTP/1.1\r\n\r\n"
            "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        ASSERT_EQ(count(res, "HTTP/1.1 200"), 2u) << "iteration " << i << "\n" << res;
    }
}

TEST_F(Http1FramingTest, ContentLengthTextInsideAnotherHeaderIsIgnored) {
    std::string res = raw_exchange(
        "GET /hello HTTP/1.1\r\nX-Note: content-length: 50\r\nConnection: close\r\n\r\n");
    EXPECT_NE(res.find("hello"), std::string::npos) << res;
}
