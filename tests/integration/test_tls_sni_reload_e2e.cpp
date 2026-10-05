#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// TLS certificates chosen by SNI, and reloaded without a restart.

namespace {

constexpr uint16_t kPort = 8144;

std::filesystem::path dir() {
    return std::filesystem::temp_directory_path() / "orbit-tls-sni-test";
}

std::string path(const std::string& name) {
    return (dir() / name).string();
}

// Writes a self-signed certificate with subject CN=@p cn and, if given, the
// subjectAltName DNS entries @p sans.
void make_cert(const std::string& cert_path, const std::string& key_path, const std::string& cn,
               const std::vector<std::string>& sans) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    if (!sans.empty()) {
        std::string value;
        for (const auto& san : sans) value += (value.empty() ? "DNS:" : ",DNS:") + san;
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, value.c_str());
        X509_add_ext(x509, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(x509, pkey, EVP_sha256());
    BIO* cert_bio = BIO_new_file(cert_path.c_str(), "wb");
    BIO* key_bio = BIO_new_file(key_path.c_str(), "wb");
    PEM_write_bio_X509(cert_bio, x509);
    PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
}

network::socket_t connect_tcp() {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return network::INVALID_SOCKET_FD;
    }
    return fd;
}

struct Handshake {
    bool ok = false;
    std::string cn;   // subject CN of the certificate the server sent
    std::string alpn; // negotiated protocol
};

// A TLS handshake asking for @p server_name ("" sends no SNI).
Handshake handshake(const std::string& server_name, bool offer_h2 = false) {
    Handshake result;
    network::socket_t fd = connect_tcp();
    if (fd == network::INVALID_SOCKET_FD) return result;
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = SSL_new(ctx);
    if (offer_h2) {
        static const unsigned char protos[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        SSL_set_alpn_protos(ssl, protos, sizeof(protos));
    }
    if (!server_name.empty()) SSL_set_tlsext_host_name(ssl, server_name.c_str());
    SSL_set_fd(ssl, static_cast<int>(fd));
    if (SSL_connect(ssl) == 1) {
        result.ok = true;
        if (X509* peer = SSL_get1_peer_certificate(ssl)) {
            char cn[256] = {};
            X509_NAME_get_text_by_NID(X509_get_subject_name(peer), NID_commonName, cn, sizeof(cn));
            result.cn = cn;
            X509_free(peer);
        }
        const unsigned char* alpn = nullptr;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
        result.alpn.assign(reinterpret_cast<const char*>(alpn), alpn_len);
        SSL_shutdown(ssl);
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    network::close_socket(fd);
    return result;
}

// Waits until a handshake for @p server_name returns certificate @p cn.
bool eventually_serves(const std::string& server_name, const std::string& cn, int timeout_ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        if (handshake(server_name).cn == cn) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

class Server {
public:
    explicit Server(config::ServerConfig cfg) : app_(std::make_unique<server::App>(cfg)) {
        app_->get("/", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });
        server::App* app = app_.get();
        thread_ = std::thread([app] { app->listen(); });
        for (int i = 0; i < 200; ++i) {
            network::socket_t fd = connect_tcp();
            if (fd != network::INVALID_SOCKET_FD) {
                network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~Server() {
        app_->stop();
        network::socket_t fd = connect_tcp();
        if (fd != network::INVALID_SOCKET_FD) network::close_socket(fd);
        if (thread_.joinable()) thread_.join();
    }
    server::App& app() { return *app_; }

private:
    std::unique_ptr<server::App> app_;
    std::thread thread_;
};

config::ServerConfig sni_config() {
    config::ServerConfig cfg;
    cfg.port = kPort;
    cfg.http_version = config::HttpVersion::Http2;
    cfg.ssl_cert = path("default.pem");
    cfg.ssl_key = path("default.key");
    cfg.sni_certificates = {
        {path("a.pem"), path("a.key")},
        {path("wild.pem"), path("wild.key")},
        {path("cn-only.pem"), path("cn-only.key")},
    };
    return cfg;
}

class TlsSniReloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::filesystem::remove_all(dir());
        std::filesystem::create_directories(dir());
        make_cert(path("default.pem"), path("default.key"), "default", {});
        make_cert(path("a.pem"), path("a.key"), "a-cert", {"a.test", "alt.a.test"});
        make_cert(path("wild.pem"), path("wild.key"), "wild-cert", {"*.wild.test"});
        make_cert(path("cn-only.pem"), path("cn-only.key"), "cn.test", {});
    }
    void TearDown() override { std::filesystem::remove_all(dir()); }
};

} // namespace

TEST_F(TlsSniReloadTest, CertificateIsChosenByServerName) {
    Server server(sni_config());
    EXPECT_EQ(handshake("a.test").cn, "a-cert");
    EXPECT_EQ(handshake("alt.a.test").cn, "a-cert");
    EXPECT_EQ(handshake("A.Test").cn, "a-cert"); // names are case-insensitive
    EXPECT_EQ(handshake("x.wild.test").cn, "wild-cert");
    EXPECT_EQ(handshake("cn.test").cn, "cn.test"); // no SAN: the CN is used
    EXPECT_EQ(handshake("other.test").cn, "default");
    EXPECT_EQ(handshake("wild.test").cn, "default");     // *.wild.test does not cover wild.test
    EXPECT_EQ(handshake("a.b.wild.test").cn, "default"); // nor two labels
    EXPECT_EQ(handshake("").cn, "default");              // no SNI at all
}

TEST_F(TlsSniReloadTest, AlpnStillNegotiatesAfterTheSwitch) {
    Server server(sni_config());
    Handshake h = handshake("a.test", true);
    ASSERT_TRUE(h.ok);
    EXPECT_EQ(h.cn, "a-cert");
    EXPECT_EQ(h.alpn, "h2");
}

TEST_F(TlsSniReloadTest, ReloadServesRenewedCertificates) {
    Server server(sni_config());
    ASSERT_EQ(handshake("a.test").cn, "a-cert");

    make_cert(path("a.pem"), path("a.key"), "a-renewed", {"a.test"});
    make_cert(path("default.pem"), path("default.key"), "default-renewed", {});
    std::string error;
    ASSERT_TRUE(server.app().reload_tls(&error)) << error;
    EXPECT_EQ(handshake("a.test").cn, "a-renewed");
    EXPECT_EQ(handshake("").cn, "default-renewed");
    EXPECT_EQ(handshake("alt.a.test").cn, "default-renewed"); // dropped from the renewed certificate
}

TEST_F(TlsSniReloadTest, FailedReloadKeepsTheCurrentCertificates) {
    Server server(sni_config());
    { std::ofstream(path("wild.pem")) << "not a certificate"; }
    std::string error;
    EXPECT_FALSE(server.app().reload_tls(&error));
    EXPECT_NE(error.find("wild.pem"), std::string::npos) << error;
    EXPECT_EQ(handshake("x.wild.test").cn, "wild-cert");
    EXPECT_EQ(handshake("a.test").cn, "a-cert");

    // A key that does not match its certificate is refused too.
    make_cert(path("wild.pem"), path("wild.key"), "wild-renewed", {"*.wild.test"});
    make_cert(path("other.pem"), path("other.key"), "other", {});
    std::filesystem::copy_file(path("other.key"), path("wild.key"), std::filesystem::copy_options::overwrite_existing);
    EXPECT_FALSE(server.app().reload_tls(&error));
    EXPECT_NE(error.find("wild.key"), std::string::npos) << error;
    EXPECT_EQ(handshake("x.wild.test").cn, "wild-cert");
}

TEST_F(TlsSniReloadTest, ChangedFilesAreReloadedOnTheInterval) {
    config::ServerConfig cfg = sni_config();
    cfg.tls_reload_interval = std::chrono::seconds(1);
    Server server(cfg);
    ASSERT_EQ(handshake("").cn, "default");
    // Some file systems keep one-second timestamps: make sure the time moves.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    make_cert(path("default.pem"), path("default.key"), "default-polled", {});
    EXPECT_TRUE(eventually_serves("", "default-polled", 5000));
}

#ifndef _WIN32
TEST_F(TlsSniReloadTest, SighupReloads) {
    Server server(sni_config());
    ASSERT_EQ(handshake("a.test").cn, "a-cert");
    make_cert(path("a.pem"), path("a.key"), "a-sighup", {"a.test"});
    std::raise(SIGHUP);
    EXPECT_TRUE(eventually_serves("a.test", "a-sighup", 3000));
    EXPECT_FALSE(server.app().is_draining()) << "SIGHUP must not shut the server down";
}
#endif

TEST(TlsContextTest, MissingFilesFailAtStartup) {
    EXPECT_THROW(network::TlsContext("/nonexistent/cert.pem", "/nonexistent/key.pem", config::HttpVersion::Http1_1),
                 std::runtime_error);
}
