#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kUpstreamPort = 8101;
constexpr uint16_t kTlsUpstreamPort = 8102;
constexpr uint16_t kProxyPort = 8103;

orbit::network::socket_t connect_to(uint16_t port) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

// Polls until something accepts TCP connections on the port.
bool wait_until_listening(uint16_t port) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        orbit::network::close_socket(fd);
        if (ok) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void send_all(orbit::network::socket_t fd, const std::string& data) {
    ::send(fd, data.data(), static_cast<int>(data.size()), 0);
}

std::string read_until(orbit::network::socket_t fd, const std::string& needle) {
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
    orbit::network::socket_t fd = connect_to(kProxyPort);
    send_all(fd, request);
    std::string res = read_until(fd, until);
    orbit::network::close_socket(fd);
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

    // BIO file APIs, not FILE*: on Windows, handing a FILE* from this CRT to
    // OpenSSL aborts with "no OPENSSL_Applink".
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

void add_upstream_routes(orbit::server::App& app) {
    app.post("/echo", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> w) {
        auto xff = req.headers.find("X-Forwarded-For");
        orbit::http::HttpResponse res;
        res.set_body("uri=" + req.uri + " page=" + (req.query.count("page") ? req.query["page"] : "") +
                     " xff=" + (xff == req.headers.end() ? "" : std::string(xff->second)) +
                     " body=" + std::string(req.body));
        w->send(std::move(res));
    });
    app.get("/chunked", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
        orbit::http::HttpResponse res;
        w->send_headers(res); // no Content-Length: chunked
        w->write_chunk("abc");
        w->write_chunk("def");
        w->end();
    });
    app.get("/hello", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
        orbit::http::HttpResponse res;
        res.set_body("secure-hello");
        w->send(std::move(res));
    });
}

std::string g_cert_path;
std::string g_key_path;

} // namespace

class ProxyE2ETest : public ::testing::Test {
protected:
    static orbit::server::App* upstream;
    static orbit::server::App* tls_upstream;
    static orbit::server::App* proxy_app;
    static std::thread t1, t2, t3;

    static void SetUpTestSuite() {
        auto dir = std::filesystem::temp_directory_path();
        g_cert_path = (dir / "orbit_proxy_test_cert.pem").string();
        g_key_path = (dir / "orbit_proxy_test_key.pem").string();
        ASSERT_TRUE(make_self_signed(g_cert_path, g_key_path));

        orbit::config::ServerConfig ucfg = orbit::test::server_config();
        ucfg.port = kUpstreamPort;
        upstream = new orbit::server::App(ucfg);
        add_upstream_routes(*upstream);

        orbit::config::ServerConfig tcfg = orbit::test::server_config();
        tcfg.port = kTlsUpstreamPort;
        tcfg.ssl_cert = g_cert_path;
        tcfg.ssl_key = g_key_path;
        tls_upstream = new orbit::server::App(tcfg);
        add_upstream_routes(*tls_upstream);

        orbit::config::ServerConfig pcfg = orbit::test::server_config();
        pcfg.port = kProxyPort;
        proxy_app = new orbit::server::App(pcfg);

        orbit::middleware::ProxyOptions plain;
        plain.target_host = "127.0.0.1";
        plain.target_port = kUpstreamPort;
        plain.strip_prefix = "/plain";
        proxy_app->use([plain](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> w) {
            static auto plain_mw = orbit::middleware::proxy(plain);
            static auto tls_unverified = [] {
                orbit::middleware::ProxyOptions o;
                o.target_host = "localhost";
                o.target_port = kTlsUpstreamPort;
                o.use_tls = true;
                o.strip_prefix = "/tls-default";
                return orbit::middleware::proxy(o);
            }();
            static auto tls_pinned = [] {
                orbit::middleware::ProxyOptions o;
                o.target_host = "localhost";
                o.target_port = kTlsUpstreamPort;
                o.use_tls = true;
                o.ca_file = g_cert_path;
                o.strip_prefix = "/tls-pinned";
                return orbit::middleware::proxy(o);
            }();
            if (req.uri.rfind("/plain", 0) == 0) return plain_mw(req, w);
            if (req.uri.rfind("/tls-default", 0) == 0) return tls_unverified(req, w);
            if (req.uri.rfind("/tls-pinned", 0) == 0) return tls_pinned(req, w);
            return true;
        });

        // Mounted under a prefix (#189): every path below it is proxied
        // without a route, and "/mountedx" is not "/mounted".
        orbit::middleware::ProxyOptions mounted;
        mounted.target_host = "127.0.0.1";
        mounted.target_port = kUpstreamPort;
        mounted.strip_prefix = "/mounted";
        proxy_app->use("/mounted", orbit::middleware::proxy(mounted));
        // The pattern from examples/basic_server.cpp: group middleware plus a
        // wildcard route, which used to be a literal "/*" and never matched.
        proxy_app->group("/wild", [](orbit::routing::Router& r) {
            orbit::middleware::ProxyOptions o;
            o.target_host = "127.0.0.1";
            o.target_port = kUpstreamPort;
            o.strip_prefix = "/wild";
            r.use(orbit::middleware::proxy(o));
            r.get("/*", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) {});
        });

        t1 = std::thread([] { upstream->listen(); });
        t2 = std::thread([] { tls_upstream->listen(); });
        t3 = std::thread([] { proxy_app->listen(); });
        // Wait until every server accepts connections. A fixed sleep was not
        // enough under Valgrind, where startup takes hundreds of milliseconds.
        for (uint16_t port : {kUpstreamPort, kTlsUpstreamPort, kProxyPort}) {
            ASSERT_TRUE(wait_until_listening(port)) << "server on port " << port << " did not start";
        }
    }

    static void TearDownTestSuite() {
        for (auto [app, port] : {std::pair{proxy_app, kProxyPort}, std::pair{upstream, kUpstreamPort}}) {
            app->stop();
            orbit::network::socket_t fd = connect_to(port);
            send_all(fd, "GET /nothing HTTP/1.1\r\nConnection: close\r\n\r\n");
            read_until(fd, "\r\n\r\n");
            orbit::network::close_socket(fd);
        }
        tls_upstream->stop();
        { orbit::network::socket_t fd = connect_to(kTlsUpstreamPort); orbit::network::close_socket(fd); }
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

orbit::server::App* ProxyE2ETest::upstream = nullptr;
orbit::server::App* ProxyE2ETest::tls_upstream = nullptr;
orbit::server::App* ProxyE2ETest::proxy_app = nullptr;
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
    orbit::network::socket_t fd = connect_to(kProxyPort);
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
    orbit::network::close_socket(fd);
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

TEST_F(ProxyE2ETest, MountedProxyForwardsAnyPathUnderItsPrefix) {
    std::string res = through_proxy("GET /mounted/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "secure-hello");
    EXPECT_NE(res.find("200"), std::string::npos) << res;
    EXPECT_NE(res.find("secure-hello"), std::string::npos) << res;
    std::string other = through_proxy("GET /mountedx/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "\r\n\r\n");
    EXPECT_NE(other.find("404"), std::string::npos) << "a sibling prefix must not be proxied: " << other;
}

TEST_F(ProxyE2ETest, WildcardGroupRouteReachesTheProxy) {
    std::string res = through_proxy("GET /wild/hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", "secure-hello");
    EXPECT_NE(res.find("secure-hello"), std::string::npos) << res;
}
