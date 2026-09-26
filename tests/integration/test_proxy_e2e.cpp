#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kUpstreamPort = 8101;
constexpr uint16_t kTlsUpstreamPort = 8102;
constexpr uint16_t kProxyPort = 8103;

network::socket_t connect_to(uint16_t port) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef _WIN32
    DWORD timeout_ms = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return fd;
}

void send_all(network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

std::string read_until(network::socket_t fd, const std::string& needle) {
    std::string out;
    char buf[2048];
    while (out.find(needle) == std::string::npos) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

std::string through_proxy(const std::string& request, const std::string& until) {
    network::socket_t fd = connect_to(kProxyPort);
    send_all(fd, request);
    std::string res = read_until(fd, until);
    network::close_socket(fd);
    return res;
}

// Writes a self-signed certificate for "localhost" and its key as PEM files.
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
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "DNS:localhost");
    X509_add_ext(x509, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x509, pkey, EVP_sha256());

    FILE* f = std::fopen(cert_path.c_str(), "wb");
    PEM_write_X509(f, x509);
    std::fclose(f);
    f = std::fopen(key_path.c_str(), "wb");
    PEM_write_PrivateKey(f, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(f);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return true;
}

void add_upstream_routes(server::App& app) {
    app.post("/echo", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
        auto xff = req.headers.find("X-Forwarded-For");
        http::HttpResponse res;
        res.set_body("uri=" + req.uri + " page=" + (req.query.count("page") ? req.query["page"] : "") +
                     " xff=" + (xff == req.headers.end() ? "" : std::string(xff->second)) +
                     " body=" + std::string(req.body));
        w->send(std::move(res));
    });
    app.get("/chunked", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
        http::HttpResponse res;
        w->send_headers(res); // no Content-Length: chunked
        w->write_chunk("abc");
        w->write_chunk("def");
        w->end();
    });
    app.get("/hello", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
        http::HttpResponse res;
        res.set_body("secure-hello");
        w->send(std::move(res));
    });
}

std::string g_cert_path;
std::string g_key_path;

} // namespace

class ProxyE2ETest : public ::testing::Test {
protected:
    static server::App* upstream;
    static server::App* tls_upstream;
    static server::App* proxy_app;
    static std::thread t1, t2, t3;

    static void SetUpTestSuite() {
        auto dir = std::filesystem::temp_directory_path();
        g_cert_path = (dir / "orbit_proxy_test_cert.pem").string();
        g_key_path = (dir / "orbit_proxy_test_key.pem").string();
        ASSERT_TRUE(make_self_signed(g_cert_path, g_key_path));

        config::ServerConfig ucfg;
        ucfg.port = kUpstreamPort;
        upstream = new server::App(ucfg);
        add_upstream_routes(*upstream);

        config::ServerConfig tcfg;
        tcfg.port = kTlsUpstreamPort;
        tcfg.ssl_cert = g_cert_path;
        tcfg.ssl_key = g_key_path;
        tls_upstream = new server::App(tcfg);
        add_upstream_routes(*tls_upstream);

        config::ServerConfig pcfg;
        pcfg.port = kProxyPort;
        proxy_app = new server::App(pcfg);

        middleware::ProxyOptions plain;
        plain.target_host = "127.0.0.1";
        plain.target_port = kUpstreamPort;
        plain.strip_prefix = "/plain";
        proxy_app->use([plain](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            static auto plain_mw = middleware::proxy(plain);
            static auto tls_unverified = [] {
                middleware::ProxyOptions o;
                o.target_host = "localhost";
                o.target_port = kTlsUpstreamPort;
                o.use_tls = true;
                o.strip_prefix = "/tls-default";
                return middleware::proxy(o);
            }();
            static auto tls_pinned = [] {
                middleware::ProxyOptions o;
                o.target_host = "localhost";
                o.target_port = kTlsUpstreamPort;
                o.use_tls = true;
                o.ca_file = g_cert_path;
                o.strip_prefix = "/tls-pinned";
                return middleware::proxy(o);
            }();
            if (req.uri.rfind("/plain", 0) == 0) return plain_mw(req, w);
            if (req.uri.rfind("/tls-default", 0) == 0) return tls_unverified(req, w);
            if (req.uri.rfind("/tls-pinned", 0) == 0) return tls_pinned(req, w);
            return true;
        });

        t1 = std::thread([] { upstream->listen(); });
        t2 = std::thread([] { tls_upstream->listen(); });
        t3 = std::thread([] { proxy_app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    static void TearDownTestSuite() {
        for (auto [app, port] : {std::pair{proxy_app, kProxyPort}, std::pair{upstream, kUpstreamPort}}) {
            app->stop();
            network::socket_t fd = connect_to(port);
            send_all(fd, "GET /nothing HTTP/1.1\r\nConnection: close\r\n\r\n");
            read_until(fd, "\r\n\r\n");
            network::close_socket(fd);
        }
        tls_upstream->stop();
        { network::socket_t fd = connect_to(kTlsUpstreamPort); network::close_socket(fd); }
        t1.join();
        t2.join();
        t3.join();
        delete proxy_app;
        delete upstream;
        delete tls_upstream;
        std::filesystem::remove(g_cert_path);
        std::filesystem::remove(g_key_path);
    }
};

server::App* ProxyE2ETest::upstream = nullptr;
server::App* ProxyE2ETest::tls_upstream = nullptr;
server::App* ProxyE2ETest::proxy_app = nullptr;
std::thread ProxyE2ETest::t1;
std::thread ProxyE2ETest::t2;
std::thread ProxyE2ETest::t3;

TEST_F(ProxyE2ETest, ForwardsBodyQueryAndOwnForwardedFor) {
    // The request is proxied only after an asynchronous DNS lookup and
    // connect; under ASan this used to read the already-freed request.
    std::string res = through_proxy(
        "POST /plain/echo?page=2 HTTP/1.1\r\nHost: public\r\nX-Forwarded-For: 6.6.6.6\r\n"
        "Content-Length: 4\r\nConnection: close\r\n\r\nping", "body=ping");
    EXPECT_NE(res.find("uri=/echo page=2 xff=127.0.0.1 body=ping"), std::string::npos) << res;
}

TEST_F(ProxyE2ETest, ChunkedUpstreamResponseIsNotDoubleEncoded) {
    network::socket_t fd = connect_to(kProxyPort);
    send_all(fd, "GET /plain/chunked HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string res = read_until(fd, "0\r\n\r\n");
    // The client sees the proxy's own chunking of "abc" and "def", not chunk
    // syntax from the upstream embedded in the body.
    EXPECT_NE(res.find("abc"), std::string::npos) << res;
    EXPECT_NE(res.find("def"), std::string::npos) << res;
    EXPECT_EQ(res.find("3\r\nabc\r\n\r\n"), std::string::npos) << res;

    // The client connection is still usable afterwards.
    send_all(fd, "POST /plain/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
    EXPECT_NE(read_until(fd, "body=ok").find("body=ok"), std::string::npos);
    network::close_socket(fd);
}

TEST_F(ProxyE2ETest, UntrustedUpstreamCertificateIsRejected) {
    std::string res = through_proxy("GET /tls-default/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "\r\n\r\n");
    EXPECT_EQ(res.find("secure-hello"), std::string::npos) << res;
    EXPECT_NE(res.find("TLS verification failed"), std::string::npos) << res;
}

TEST_F(ProxyE2ETest, PinnedCaAllowsUpstream) {
    std::string res = through_proxy("GET /tls-pinned/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "secure-hello");
    EXPECT_NE(res.find("secure-hello"), std::string::npos) << res;
}
