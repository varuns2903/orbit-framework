#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

using namespace http;

namespace {

constexpr uint16_t kPlainPort = 8115;
constexpr uint16_t kTlsPort = 8116;

std::mutex g_mutex;
std::string g_received;
bool g_ended = false;

network::socket_t connect_local(uint16_t port) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return network::INVALID_SOCKET_FD;
    }
    return fd;
}

void set_recv_timeout(network::socket_t fd, int ms) {
#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void send_all(network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

std::string read_until_close(network::socket_t fd) {
    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

bool make_self_signed(const std::string& cert_path, const std::string& key_path) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x509 = X509_new();
    if (!pkey || !x509) return false;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());
    // BIO file APIs, not FILE*: on Windows a FILE* from this CRT aborts OpenSSL.
    BIO* cert_bio = BIO_new_file(cert_path.c_str(), "wb");
    BIO* key_bio = BIO_new_file(key_path.c_str(), "wb");
    bool ok = cert_bio && key_bio && PEM_write_bio_X509(cert_bio, x509) == 1 &&
              PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return ok;
}

// Starts an App on its own thread; stops it and wakes its loop on destruction.
struct ServerThread {
    server::App* app;
    std::thread thread;
    uint16_t port;
    bool tls;

    ServerThread(server::App* a, uint16_t p, bool use_tls) : app(a), port(p), tls(use_tls) {
        thread = std::thread([this] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            network::socket_t fd = connect_local(port);
            if (fd != network::INVALID_SOCKET_FD) {
                network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~ServerThread() {
        app->stop();
        network::socket_t fd = connect_local(port); // wake the loop
        if (fd != network::INVALID_SOCKET_FD) network::close_socket(fd);
        if (thread.joinable()) thread.join();
        delete app;
    }
};

} // namespace

// A handler that answers with a fixed-length response while a chunked
// request body is still arriving. The response flag used to be shared with
// the request decoder, so the rest of the body was read as raw bytes.
TEST(ConnectionStateTest, ResponseFramingDoesNotChangeRequestDecoding) {
    config::ServerConfig cfg;
    cfg.port = kPlainPort;
    auto* app = new server::App(cfg);
    app->group("/s", [](routing::Router& r) {
        r.add_stream_route(HttpMethod::POST, "/up", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse head;
            head.headers["Content-Length"] = "2";
            head.headers["Content-Type"] = "text/plain";
            w->send_headers(head); // not chunked, unlike the request
            auto body = std::make_shared<std::string>();
            w->read_body_stream(
                [body](std::string_view data) { body->append(data); },
                [w, body]() {
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_received = *body;
                        g_ended = true;
                    }
                    w->write_chunk("ok");
                    w->end();
                });
        });
    });
    ServerThread server(app, kPlainPort, false);

    network::socket_t fd = connect_local(kPlainPort);
    ASSERT_NE(fd, network::INVALID_SOCKET_FD);
    set_recv_timeout(fd, 3000);
    send_all(fd, "POST /s/up HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                 "5\r\nhello\r\n");
    // Let the handler send its headers before the rest of the body arrives.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    send_all(fd, "6\r\n world\r\n0\r\n\r\n");
    std::string res = read_until_close(fd);
    network::close_socket(fd);

    EXPECT_EQ(res.rfind("HTTP/1.1 200", 0), 0u) << res;
    EXPECT_NE(res.find("\r\n\r\nok"), std::string::npos) << res;
    std::lock_guard<std::mutex> lock(g_mutex);
    EXPECT_TRUE(g_ended);
    EXPECT_EQ(g_received, "hello world");
}

// A client that ends TLS with close_notify but leaves TCP open must not
// hold the connection until the keep-alive timeout.
TEST(ConnectionStateTest, TlsCloseNotifyClosesTheConnection) {
    auto dir = std::filesystem::temp_directory_path();
    std::string cert = (dir / "orbit_conn_state_cert.pem").string();
    std::string key = (dir / "orbit_conn_state_key.pem").string();
    ASSERT_TRUE(make_self_signed(cert, key));

    config::ServerConfig cfg;
    cfg.port = kTlsPort;
    cfg.ssl_cert = cert;
    cfg.ssl_key = key;
    cfg.keep_alive_timeout = std::chrono::seconds(30);
    auto* app = new server::App(cfg);
    app->get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.set_body("ok");
        w->send(std::move(res));
    });
    {
        ServerThread server(app, kTlsPort, true);

        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        ASSERT_NE(ctx, nullptr);
        network::socket_t fd = connect_local(kTlsPort);
        ASSERT_NE(fd, network::INVALID_SOCKET_FD);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, static_cast<int>(fd));
        ASSERT_EQ(SSL_connect(ssl), 1);

        std::string req = "GET /ok HTTP/1.1\r\nHost: x\r\n\r\n";
        ASSERT_GT(SSL_write(ssl, req.data(), static_cast<int>(req.size())), 0);
        std::string res;
        char buf[4096];
        while (res.find("\r\n\r\nok") == std::string::npos) {
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) break;
            res.append(buf, static_cast<size_t>(n));
        }
        EXPECT_NE(res.find("\r\n\r\nok"), std::string::npos) << res;

        // close_notify, TCP stays open.
        SSL_shutdown(ssl);
        set_recv_timeout(fd, 3000);
        auto start = std::chrono::steady_clock::now();
        read_until_close(fd);
        auto waited = std::chrono::steady_clock::now() - start;
        EXPECT_LT(waited, std::chrono::milliseconds(2500)) << "server kept the connection after close_notify";

        SSL_free(ssl);
        SSL_CTX_free(ctx);
        network::close_socket(fd);
    }
    std::filesystem::remove(cert);
    std::filesystem::remove(key);
}
