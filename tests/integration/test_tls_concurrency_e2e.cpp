#include <gtest/gtest.h>
#include <orbit/server/App.hpp>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kPort = 8105;
std::string g_cert, g_key;

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

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

} // namespace

class TlsConcurrencyTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        auto dir = std::filesystem::temp_directory_path();
        g_cert = (dir / "orbit_tls_conc_cert.pem").string();
        g_key = (dir / "orbit_tls_conc_key.pem").string();
        ASSERT_TRUE(make_self_signed(g_cert, g_key));

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.ssl_cert = g_cert;
        cfg.ssl_key = g_key;
        cfg.http_version = config::HttpVersion::Http2;
        cfg.worker_threads = 8;
        app = new server::App(cfg);
        app->post("/echo", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body(std::string(req.body.size() * 16, 'r'));
            w->send(std::move(res));
        });
        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    static void TearDownTestSuite() {
        app->stop();
        CURL* c = curl_easy_init();
        std::string url = "https://localhost:" + std::to_string(kPort) + "/";
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 500L);
        curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (server_thread.joinable()) server_thread.join();
        delete app;
        std::filesystem::remove(g_cert);
        std::filesystem::remove(g_key);
    }
};

server::App* TlsConcurrencyTest::app = nullptr;
std::thread TlsConcurrencyTest::server_thread;

// Many multiplexed streams on one TLS connection: the event loop encrypts
// flow-control frames while worker threads encrypt responses, so both
// touch the connection's TLS state at the same time.
TEST_F(TlsConcurrencyTest, MultiplexedStreamsOnOneTlsConnection) {
    const int kStreams = 100;
    const std::string payload(20000, 'q');
    std::string url = "https://localhost:" + std::to_string(kPort) + "/echo";

    for (int round = 0; round < 6; ++round) {
        CURLM* multi = curl_multi_init();
        curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 1L);

        std::vector<CURL*> handles;
        std::vector<std::string> bodies(kStreams);
        for (int i = 0; i < kStreams; ++i) {
            CURL* h = curl_easy_init();
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
            curl_easy_setopt(h, CURLOPT_PIPEWAIT, 1L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 15000L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, &bodies[static_cast<size_t>(i)]);
            curl_multi_add_handle(multi, h);
            handles.push_back(h);
        }

        int running = 0;
        do {
            curl_multi_perform(multi, &running);
            curl_multi_poll(multi, nullptr, 0, 100, nullptr);
        } while (running > 0);

        int ok = 0;
        CURLMsg* msg;
        int left;
        while ((msg = curl_multi_info_read(multi, &left))) {
            if (msg->msg == CURLMSG_DONE && msg->data.result == CURLE_OK) ++ok;
        }
        for (CURL* h : handles) {
            curl_multi_remove_handle(multi, h);
            curl_easy_cleanup(h);
        }
        curl_multi_cleanup(multi);

        EXPECT_EQ(ok, kStreams) << "round " << round;
        for (const auto& b : bodies) EXPECT_EQ(b.size(), payload.size() * 16);
    }
}
