#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

// HTTP/2 over plaintext with prior knowledge (h2c, RFC 9113 section 3.3).

namespace {

constexpr uint16_t kH2Port = 8142; // http_version = Http2
constexpr uint16_t kH1Port = 8143; // http_version = Http1_1: no h2c

const std::string kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const std::string kEmptySettings("\x00\x00\x00\x04\x00\x00\x00\x00\x00", 9);

orbit::network::socket_t connect_to(uint16_t port, int timeout_ms = 3000) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        orbit::network::close_socket(fd);
        return orbit::network::INVALID_SOCKET_FD;
    }
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_str(orbit::network::socket_t fd, const std::string& s) {
    ::send(fd, s.data(), static_cast<int>(s.size()), 0);
}

std::string read_some(orbit::network::socket_t fd) {
    char buf[4096];
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

struct Result {
    CURLcode code;
    long status;
    long version;
    std::string body;
};

Result get(uint16_t port, const std::string& path, long http_version) {
    Result r{};
    CURL* c = curl_easy_init();
    std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, http_version);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    r.code = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &r.version);
    curl_easy_cleanup(c);
    return r;
}

class Server {
public:
    Server(uint16_t port, orbit::config::HttpVersion version) {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = port;
        cfg.http_version = version;
        app_ = std::make_unique<orbit::server::App>(cfg);
        app_->get("/hello", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            res.set_body("hello q=" + req.query["q"]);
            w->send(std::move(res));
        });
        app_->get("/stream", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            w->send_headers(res);
            w->write_chunk("a,");
            w->write_chunk("b,");
            w->write_chunk("c");
            w->end();
        });
        orbit::server::App* app = app_.get();
        thread_ = std::thread([app] { app->listen(); });
        for (int i = 0; i < 200; ++i) {
            orbit::network::socket_t fd = connect_to(port);
            if (fd != orbit::network::INVALID_SOCKET_FD) {
                orbit::network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        port_ = port;
    }
    ~Server() {
        app_->stop();
        orbit::network::socket_t fd = connect_to(port_); // wake the loop
        if (fd != orbit::network::INVALID_SOCKET_FD) orbit::network::close_socket(fd);
        if (thread_.joinable()) thread_.join();
    }

private:
    std::unique_ptr<orbit::server::App> app_;
    std::thread thread_;
    uint16_t port_ = 0;
};

bool curl_has_http2() {
    return (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2) != 0;
}

} // namespace

TEST(H2cTest, PriorKnowledgeClientGetsHttp2) {
    if (!curl_has_http2()) GTEST_SKIP() << "libcurl was built without HTTP/2 support";
    Server server(kH2Port, orbit::config::HttpVersion::Http2);
    auto r = get(kH2Port, "/hello?q=h2c", CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.version, static_cast<long>(CURL_HTTP_VERSION_2_0));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "hello q=h2c");

    auto s = get(kH2Port, "/stream", CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    ASSERT_EQ(s.code, CURLE_OK) << curl_easy_strerror(s.code);
    EXPECT_EQ(s.version, static_cast<long>(CURL_HTTP_VERSION_2_0));
    EXPECT_EQ(s.body, "a,b,c");

    // HTTP/1.1 clients are still served on the same port.
    auto h1 = get(kH2Port, "/hello?q=h1", CURL_HTTP_VERSION_1_1);
    ASSERT_EQ(h1.code, CURLE_OK) << curl_easy_strerror(h1.code);
    EXPECT_EQ(h1.version, static_cast<long>(CURL_HTTP_VERSION_1_1));
    EXPECT_EQ(h1.body, "hello q=h1");
}

TEST(H2cTest, PrefaceSplitAcrossWritesIsRecognised) {
    Server server(kH2Port, orbit::config::HttpVersion::Http2);
    orbit::network::socket_t fd = connect_to(kH2Port);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, kPreface.substr(0, 5)); // "PRI *": also a valid HTTP/1.1 start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_str(fd, kPreface.substr(5) + kEmptySettings);
    std::string reply;
    for (std::string chunk = read_some(fd); !chunk.empty() && reply.size() < 9; chunk = read_some(fd)) reply += chunk;
    orbit::network::close_socket(fd);
    ASSERT_GE(reply.size(), 9u);
    EXPECT_EQ(static_cast<uint8_t>(reply[3]), 0x04) << "expected the server's SETTINGS frame";
    EXPECT_EQ(reply.rfind("HTTP/1.1", 0), std::string::npos) << reply;
}

TEST(H2cTest, Http1OnlyServerDoesNotSpeakHttp2) {
    Server server(kH1Port, orbit::config::HttpVersion::Http1_1);
    orbit::network::socket_t fd = connect_to(kH1Port);
    ASSERT_NE(fd, orbit::network::INVALID_SOCKET_FD);
    send_str(fd, kPreface + kEmptySettings);
    std::string reply = read_some(fd);
    orbit::network::close_socket(fd);
    EXPECT_EQ(reply.rfind("HTTP/1.1 ", 0), 0u) << "got: " << reply;
}
