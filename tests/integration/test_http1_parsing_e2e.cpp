#include <gtest/gtest.h>

#ifndef _WIN32
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

// HTTP/1.1 request handling on the llhttp-based parser, over real sockets:
// requests split across writes, keep-alive after a streamed upload, stream
// routes with and without a body, body limits, and timeouts.

using namespace orbit::http;

namespace {

constexpr int kPort = 8156;
constexpr size_t kMaxBody = 1024;

int connect_client() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{0, 200 * 1000}; // recv waits at most 200 ms at a time
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// Reads until `responses` complete responses have arrived (each with a
// Content-Length), the peer closes, or `limit` passes.
struct Read {
    std::string data;
    bool closed = false;
};

size_t complete_responses(const std::string& data) {
    size_t count = 0, pos = 0;
    while (true) {
        size_t head_end = data.find("\r\n\r\n", pos);
        if (head_end == std::string::npos) break;
        size_t cl = data.find("Content-Length: ", pos);
        size_t len = 0;
        if (cl != std::string::npos && cl < head_end) len = std::stoul(data.substr(cl + 16));
        if (data.size() < head_end + 4 + len) break;
        ++count;
        pos = head_end + 4 + len;
    }
    return count;
}

Read read_responses(int fd, size_t responses, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    Read r;
    auto deadline = std::chrono::steady_clock::now() + limit;
    char buf[8192];
    while (std::chrono::steady_clock::now() < deadline && complete_responses(r.data) < responses) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            r.data.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            r.closed = true;
            break;
        }
    }
    return r;
}

size_t count(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + 1)) ++n;
    return n;
}

} // namespace

class Http1ParsingE2ETest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        cfg.max_body_size = kMaxBody;
        cfg.header_timeout = std::chrono::seconds(1);
        app = std::make_unique<orbit::server::App>(cfg);

        app->post("/echo", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("echo:" + std::string(req.body), "text/plain");
            w->send(std::move(res));
        });
        app->get("/hello/:name", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("hello " + req.uri, "text/plain");
            w->send(std::move(res));
        });
        app->group("/s", [](orbit::routing::Router& r) {
            // Counts the streamed body and answers when it ends.
            r.add_stream_route(HttpMethod::POST, "/count", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
                auto total = std::make_shared<size_t>(0);
                w->read_body_stream(
                    [total](std::string_view piece) { *total += piece.size(); },
                    [w, total] {
                        HttpResponse res;
                        res.set_body("streamed " + std::to_string(*total), "text/plain");
                        w->send(std::move(res));
                    });
            });
            // Answers at once without reading the body.
            r.add_stream_route(HttpMethod::POST, "/early", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
                HttpResponse res;
                res.set_body("not reading that", "text/plain");
                w->send(std::move(res));
            });
        });

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            int fd = connect_client();
            if (fd >= 0) {
                ::close(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
    }
};

std::unique_ptr<orbit::server::App> Http1ParsingE2ETest::app;
std::thread Http1ParsingE2ETest::server_thread;

TEST_F(Http1ParsingE2ETest, RequestSentOneByteAtATime) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    const std::string req = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nabcde";
    for (char c : req) send_all(fd, std::string(1, c));
    Read r = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(r.data.find("200 OK"), std::string::npos) << r.data;
    EXPECT_NE(r.data.find("echo:abcde"), std::string::npos) << r.data;
}

TEST_F(Http1ParsingE2ETest, PipelinedRequestsInOneWriteAreAnsweredInOrder) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "GET /hello/1 HTTP/1.1\r\nHost: h\r\n\r\n"
                 "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nxyz\r\n0\r\n\r\n"
                 "GET /hello/3 HTTP/1.1\r\nHost: h\r\n\r\n");
    Read r = read_responses(fd, 3);
    ::close(fd);
    size_t a = r.data.find("hello /hello/1"), b = r.data.find("echo:xyz"), c = r.data.find("hello /hello/3");
    ASSERT_NE(a, std::string::npos) << r.data;
    ASSERT_NE(b, std::string::npos) << r.data;
    ASSERT_NE(c, std::string::npos) << r.data;
    EXPECT_LT(a, b);
    EXPECT_LT(b, c);
}

// The connection used to stay in its streaming state after an upload, so the
// next request on it was never answered.
TEST_F(Http1ParsingE2ETest, KeepAliveContinuesAfterAStreamedUpload) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /s/count HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n0123456789");
    Read first = read_responses(fd, 1);
    EXPECT_NE(first.data.find("streamed 10"), std::string::npos) << first.data;

    send_all(fd, "GET /hello/after HTTP/1.1\r\nHost: h\r\n\r\n");
    Read second = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(second.data.find("hello /hello/after"), std::string::npos) << second.data;
}

TEST_F(Http1ParsingE2ETest, ChunkedStreamedUploadIsDecoded) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /s/count HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "4;ext=1\r\nabcd\r\n6\r\nefghij\r\n0\r\nX-Trailer: t\r\n\r\n");
    Read r = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(r.data.find("streamed 10"), std::string::npos) << r.data;
}

// A body over max_body_size is refused on ordinary routes but streamed on
// stream routes, which exist for large uploads.
TEST_F(Http1ParsingE2ETest, BodyLimitAppliesToOrdinaryRoutesOnly) {
    const std::string big(64 * 1024, 'z');
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /s/count HTTP/1.1\r\nHost: h\r\nContent-Length: " + std::to_string(big.size()) + "\r\n\r\n" + big);
    Read streamed = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(streamed.data.find("streamed 65536"), std::string::npos) << streamed.data.substr(0, 200);

    fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: " + std::to_string(kMaxBody + 1) + "\r\n\r\n");
    Read refused = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(refused.data.find("413"), std::string::npos) << refused.data;
}

// Without Content-Length or Transfer-Encoding a request has no body (RFC
// 9112 section 6.3), so the stream ends at once.
TEST_F(Http1ParsingE2ETest, StreamRouteWithoutABodyEndsAtOnce) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /s/count HTTP/1.1\r\nHost: h\r\n\r\n");
    Read r = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(r.data.find("streamed 0"), std::string::npos) << r.data;
}

// The handler answered without reading the body: what is left of it is
// still on the wire, so the connection closes after the response rather
// than parse it as a request.
TEST_F(Http1ParsingE2ETest, UnreadStreamedBodyClosesTheConnectionAfterTheResponse) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /s/early HTTP/1.1\r\nHost: h\r\nContent-Length: 100000\r\n\r\nGET /smuggled HTTP/1.1\r\n\r\n");
    Read r = read_responses(fd, 2, std::chrono::seconds(3));
    ::close(fd);
    EXPECT_NE(r.data.find("not reading that"), std::string::npos) << r.data;
    EXPECT_EQ(r.data.find("/smuggled"), std::string::npos) << "body bytes were parsed as a request";
    EXPECT_EQ(count(r.data, "HTTP/1.1 "), 1u) << r.data;
}

TEST_F(Http1ParsingE2ETest, UnknownMethodIs501) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "BREW /pot HTTP/1.1\r\nHost: h\r\n\r\n");
    Read r = read_responses(fd, 1);
    ::close(fd);
    EXPECT_NE(r.data.find("501"), std::string::npos) << r.data;
}

TEST_F(Http1ParsingE2ETest, SmuggledFramingIs400AndCloses) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "0\r\n\r\nGET /hello/smuggled HTTP/1.1\r\nHost: h\r\n\r\n");
    Read r = read_responses(fd, 2, std::chrono::seconds(3));
    ::close(fd);
    EXPECT_NE(r.data.find("400"), std::string::npos) << r.data;
    EXPECT_EQ(r.data.find("hello /hello/smuggled"), std::string::npos) << r.data;
}

// Trickling header bytes must not extend header_timeout (1 s here): the
// parser consumes them as they come, so an empty buffer no longer means a
// new request.
TEST_F(Http1ParsingE2ETest, TrickledHeadersStillTimeOut) {
    int fd = connect_client();
    ASSERT_GE(fd, 0);
    send_all(fd, "GET /hello/slow HTTP/1.1\r\n");
    auto start = std::chrono::steady_clock::now();
    bool closed = false;
    char buf[256];
    for (int i = 0; i < 16 && !closed; ++i) { // up to ~4 s
        send_all(fd, "X");
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0); // waits up to 200 ms
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) closed = true;
        else if (n > 0) closed = true; // an error response before closing
    }
    auto took = std::chrono::steady_clock::now() - start;
    ::close(fd);
    EXPECT_TRUE(closed) << "the server kept waiting for headers";
    EXPECT_LT(took, std::chrono::milliseconds(2500)) << "header_timeout was extended by trickled bytes";
}

#endif
