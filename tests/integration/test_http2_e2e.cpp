#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

constexpr uint16_t kPort = 8104;
std::string g_cert, g_key, g_file, g_file_content;
std::atomic<int> g_sentinel_survived{-1};

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

struct H2Result {
    CURLcode code;
    long status;
    long http_version;
    std::string body;
};

H2Result h2_request(const std::string& path, const std::string& post_body = "", long timeout_ms = 5000) {
    H2Result r{};
    CURL* curl = curl_easy_init();
    std::string url = "https://localhost:" + std::to_string(kPort) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    if (!post_body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_body.size()));
    }
    r.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &r.http_version);
    curl_easy_cleanup(curl);
    return r;
}

} // namespace

class Http2E2ETest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        auto dir = std::filesystem::temp_directory_path();
        g_cert = (dir / "orbit_h2_cert.pem").string();
        g_key = (dir / "orbit_h2_key.pem").string();
        g_file = (dir / "orbit_h2_payload.bin").string();
        ASSERT_TRUE(make_self_signed(g_cert, g_key));
        for (int i = 0; i < 100000; ++i) g_file_content += "line " + std::to_string(i) + "\n";
        std::ofstream(g_file, std::ios::binary) << g_file_content;

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.ssl_cert = g_cert;
        cfg.ssl_key = g_key;
        cfg.http_version = config::HttpVersion::Http2;
        cfg.max_body_size = 1024;
        app = new server::App(cfg);

        app->get("/file", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.send_file(g_file, "application/octet-stream");
            w->send(std::move(res));
        });
#ifndef _WIN32
        // Opens a file right after send(). The descriptor number the stream
        // just released is reused for it, so a second close() of the response's
        // file descriptor would close this unrelated file.
        app->get("/file-then-open", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            int sentinel = -1;
            {
                http::HttpResponse res;
                res.send_file(g_file, "application/octet-stream");
                w->send(std::move(res));
                sentinel = ::open(g_file.c_str(), O_RDONLY);
            } // res is destroyed here
            g_sentinel_survived = (sentinel != -1 && ::fcntl(sentinel, F_GETFD) != -1) ? 1 : 0;
            if (sentinel != -1) ::close(sentinel);
        });
#endif
        app->post("/echo", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("got " + std::to_string(req.body.size()));
            w->send(std::move(res));
        });
        app->get("/slow", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            http::HttpResponse res;
            res.set_body("late");
            w->send(std::move(res)); // the client has already gone
        });
        app->get("/ok", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    static void TearDownTestSuite() {
        app->stop();
        h2_request("/ok", "", 500);
        if (server_thread.joinable()) server_thread.join();
        delete app;
        std::filesystem::remove(g_cert);
        std::filesystem::remove(g_key);
        std::filesystem::remove(g_file);
    }
};

server::App* Http2E2ETest::app = nullptr;
std::thread Http2E2ETest::server_thread;

TEST_F(Http2E2ETest, NegotiatesHttp2) {
    auto r = h2_request("/ok");
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.http_version, static_cast<long>(CURL_HTTP_VERSION_2_0));
    EXPECT_EQ(r.body, "ok");
}

TEST_F(Http2E2ETest, ServesFilesIntact) {
    // The file descriptor used to be closed as soon as send() returned,
    // before the data provider read it.
    for (int i = 0; i < 5; ++i) {
        auto r = h2_request("/file");
        ASSERT_EQ(r.code, CURLE_OK) << "attempt " << i << ": " << curl_easy_strerror(r.code);
        EXPECT_EQ(r.status, 200);
        EXPECT_EQ(r.body.size(), g_file_content.size());
        EXPECT_TRUE(r.body == g_file_content) << "attempt " << i;
    }
}

#ifndef _WIN32
TEST_F(Http2E2ETest, ResponseFileDescriptorIsClosedExactlyOnce) {
    g_sentinel_survived = -1;
    auto r = h2_request("/file-then-open");
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.body.size(), g_file_content.size());
    for (int i = 0; i < 100 && g_sentinel_survived == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(g_sentinel_survived.load(), 1) << "an unrelated file descriptor was closed";
}
#endif

TEST_F(Http2E2ETest, OversizedBodyGets413) {
    auto small = h2_request("/echo", std::string(100, 'a'));
    ASSERT_EQ(small.code, CURLE_OK);
    EXPECT_EQ(small.body, "got 100");

    auto big = h2_request("/echo", std::string(8192, 'b'));
    ASSERT_EQ(big.code, CURLE_OK) << curl_easy_strerror(big.code);
    EXPECT_EQ(big.status, 413);
}

TEST_F(Http2E2ETest, HandlerFinishingAfterClientLeftIsSafe) {
    auto r = h2_request("/slow", "", 150); // gives up before the handler replies
    EXPECT_NE(r.code, CURLE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(900)); // let the handler finish
    auto after = h2_request("/ok");
    ASSERT_EQ(after.code, CURLE_OK);
    EXPECT_EQ(after.body, "ok");
}
