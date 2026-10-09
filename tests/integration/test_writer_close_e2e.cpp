#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <curl/curl.h>
#include <nghttp2/nghttp2.h>
#include "../utils/TestConfig.hpp"

#include <chrono>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ResponseWriter::is_open() and on_close() (#197): a long-lived writer, such
// as an SSE subscriber kept by a hub, learns that its client has gone, so
// the hub can drop it instead of writing to it forever.

using namespace orbit::http;

namespace {

constexpr uint16_t kPort = 8175;

std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<std::shared_ptr<ResponseWriter>> g_subscribers; // the "hub"
std::vector<std::string> g_closed;                          // http_version of each closed subscriber

bool wait_for_closed(size_t n) {
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_cv.wait_for(lock, std::chrono::seconds(5), [n] { return g_closed.size() >= n; });
}

size_t discard(void*, size_t size, size_t n, void*) { return size * n; }

} // namespace

class WriterCloseTest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        cfg.http_version = orbit::config::HttpVersion::Http2; // HTTP/1.1, and h2c with prior knowledge
        app = std::make_unique<orbit::server::App>(cfg);

        app->get("/ping", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            w->send(HttpResponse().send("ok"));
        });
        app->get("/events", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            // Subscribe before writing anything: once the first event is out,
            // the client may look at the hub (a race seen on Windows).
            std::string version = req.http_version;
            // Capture the writer weakly: the hub owns it, the callback must not.
            std::weak_ptr<ResponseWriter> weak = w;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_subscribers.push_back(w);
            }
            w->on_close([weak, version] {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (auto writer = weak.lock()) {
                    std::erase(g_subscribers, writer);
                }
                g_closed.push_back(version);
                g_cv.notify_all();
            });
            HttpResponse res;
            res.headers["Content-Type"] = "text/event-stream";
            res.headers["Cache-Control"] = "no-cache";
            w->send_headers(res);
            w->send_sse_event("hello");
        });

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            CURL* c = curl_easy_init();
            std::string url = "http://127.0.0.1:" + std::to_string(kPort) + "/ping";
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
            CURLcode code = curl_easy_perform(c);
            curl_easy_cleanup(c);
            if (code == CURLE_OK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_subscribers.clear();
    }

    void SetUp() override {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_closed.clear();
        g_subscribers.clear();
    }
};

std::unique_ptr<orbit::server::App> WriterCloseTest::app;
std::thread WriterCloseTest::server_thread;

TEST_F(WriterCloseTest, Http1ClientLeavingClosesTheWriter) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
#ifdef _WIN32
    DWORD timeout_ms = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    const std::string req = "GET /events HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(fd, req.data(), static_cast<int>(req.size()), 0);
    std::string got;
    char buf[1024];
    while (got.find("data: hello") == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        got.append(buf, static_cast<size_t>(n));
    }
    ASSERT_NE(got.find("data: hello"), std::string::npos) << got;

    std::shared_ptr<ResponseWriter> writer;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ASSERT_EQ(g_subscribers.size(), 1u);
        writer = g_subscribers.front();
    }
    EXPECT_TRUE(writer->is_open());

    orbit::network::close_socket(fd);
    ASSERT_TRUE(wait_for_closed(1)) << "on_close never ran";
    EXPECT_EQ(g_closed[0], "HTTP/1.1");
    EXPECT_FALSE(writer->is_open());
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        EXPECT_TRUE(g_subscribers.empty()) << "the callback removed the subscriber";
    }
    writer->send_sse_event("into the void"); // dropped, never a crash

    // Registered after the close: runs at once.
    bool late = false;
    writer->on_close([&late] { late = true; });
    EXPECT_TRUE(late);
}

namespace {

// A minimal h2c client (prior knowledge) on nghttp2, so this runs whether or
// not libcurl was built with HTTP/2: GET /events, wait for the first DATA
// frame, then leave.
struct H2Client {
    orbit::network::socket_t fd = orbit::network::INVALID_SOCKET_FD;
    nghttp2_session* session = nullptr;
    bool got_data = false;

    static ssize_t send_cb(nghttp2_session*, const uint8_t* data, size_t len, int, void* user) {
        auto* self = static_cast<H2Client*>(user);
        auto n = ::send(self->fd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
        return n < 0 ? NGHTTP2_ERR_CALLBACK_FAILURE : static_cast<ssize_t>(n);
    }
    static int data_cb(nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void* user) {
        static_cast<H2Client*>(user)->got_data = true;
        return 0;
    }

    bool get_first_event(const std::string& path) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPort);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
#ifdef _WIN32
        DWORD timeout_ms = 3000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        nghttp2_session_callbacks* callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_send_callback(callbacks, send_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, data_cb);
        nghttp2_session_client_new(&session, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);

        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
        auto nv = [](const char* name, const std::string& value) {
            return nghttp2_nv{reinterpret_cast<uint8_t*>(const_cast<char*>(name)),
                              reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())), std::strlen(name),
                              value.size(), NGHTTP2_NV_FLAG_NONE};
        };
        const std::string method = "GET", scheme = "http", authority = "127.0.0.1";
        nghttp2_nv headers[] = {nv(":method", method), nv(":scheme", scheme), nv(":authority", authority),
                                nv(":path", path)};
        nghttp2_submit_request(session, nullptr, headers, 4, nullptr, nullptr);
        if (nghttp2_session_send(session) != 0) return false;

        char buf[4096];
        while (!got_data) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return false;
            if (nghttp2_session_mem_recv(session, reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n)) < 0) {
                return false;
            }
            nghttp2_session_send(session); // SETTINGS ACK and the like
        }
        return true;
    }

    ~H2Client() {
        if (session) nghttp2_session_del(session);
        if (fd != orbit::network::INVALID_SOCKET_FD) orbit::network::close_socket(fd);
    }
};

} // namespace

TEST_F(WriterCloseTest, Http2ClientLeavingClosesTheStreamWriter) {
    {
        H2Client client;
        ASSERT_TRUE(client.get_first_event("/events"));
        std::lock_guard<std::mutex> lock(g_mutex);
        ASSERT_EQ(g_subscribers.size(), 1u);
        EXPECT_TRUE(g_subscribers.front()->is_open());
    } // the client leaves: the connection, and with it the stream, closes

    ASSERT_TRUE(wait_for_closed(1)) << "on_close never ran";
    EXPECT_EQ(g_closed[0], "HTTP/2");
    std::lock_guard<std::mutex> lock(g_mutex);
    EXPECT_TRUE(g_subscribers.empty());
}
