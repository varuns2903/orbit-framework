#include <gtest/gtest.h>
#include <orbit/server/App.hpp>

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

constexpr uint16_t kPort = 8110;
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

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

struct Result {
    CURLcode code;
    long status;
    long version;
    std::string headers;
    std::string body;
};

Result h2(const std::string& path, const std::string& cookie = "", const std::string& post = "") {
    Result r{};
    CURL* c = curl_easy_init();
    std::string url = "https://localhost:" + std::to_string(kPort) + path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    if (!cookie.empty()) curl_easy_setopt(c, CURLOPT_COOKIE, cookie.c_str());
    if (!post.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(post.size()));
    }
    r.code = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &r.version);
    curl_easy_cleanup(c);
    return r;
}

} // namespace

class Http2ParityTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        auto dir = std::filesystem::temp_directory_path();
        g_cert = (dir / "orbit_h2p_cert.pem").string();
        g_key = (dir / "orbit_h2p_key.pem").string();
        ASSERT_TRUE(make_self_signed(g_cert, g_key));

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.ssl_cert = g_cert;
        cfg.ssl_key = g_key;
        cfg.http_version = config::HttpVersion::Http2;
        app = new server::App(cfg);

        app->use([](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            w->add_interceptor([](http::HttpResponse& res) { res.headers["X-Intercepted"] = "yes"; });
            return true;
        });
        app->get("/whoami", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("uri=" + req.uri + " q=" + req.query["q"] + " session=" + req.cookies["session"] +
                         " theme=" + req.cookies["theme"]);
            w->send(std::move(res));
        });
        app->get("/stream", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            w->send_headers(res);
            w->write_chunk("one,");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            w->write_chunk("two,");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            w->write_chunk("three");
            w->end();
        });
        app->get("/sse", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.headers["Content-Type"] = "text/event-stream";
            w->send_headers(res);
            w->send_sse_event("hello\nworld", "greet", "1");
            w->end();
        });
        app->post("/upload", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            auto received = std::make_shared<std::string>();
            w->read_body_stream(
                [received](std::string_view chunk) { received->append(chunk); },
                [received, w]() {
                    http::HttpResponse res;
                    res.set_body("got " + std::to_string(received->size()));
                    w->send(std::move(res));
                });
        });

        server_thread = std::thread([] { app->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    void SetUp() override {
        // Without HTTP/2 in libcurl these requests silently fall back to
        // HTTP/1.1 and would no longer test the HTTP/2 code at all.
        if (!(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP2)) {
            GTEST_SKIP() << "libcurl was built without HTTP/2 support";
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        h2("/whoami");
        if (server_thread.joinable()) server_thread.join();
        delete app;
        std::filesystem::remove(g_cert);
        std::filesystem::remove(g_key);
    }
};

server::App* Http2ParityTest::app = nullptr;
std::thread Http2ParityTest::server_thread;

TEST_F(Http2ParityTest, QueryCookiesAndInterceptors) {
    auto r = h2("/whoami?q=hello&x=1", "session=abc; theme=dark");
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.version, static_cast<long>(CURL_HTTP_VERSION_2_0));
    EXPECT_EQ(r.status, 200); // used to 404: the query string was part of the route key
    EXPECT_EQ(r.body, "uri=/whoami q=hello session=abc theme=dark");
    EXPECT_NE(r.headers.find("x-intercepted: yes"), std::string::npos) << r.headers;
}

TEST_F(Http2ParityTest, WriteChunkStreamsTheBody) {
    auto r = h2("/stream");
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.body, "one,two,three");
    EXPECT_NE(r.headers.find("x-intercepted: yes"), std::string::npos) << r.headers;
}

TEST_F(Http2ParityTest, ServerSentEvents) {
    auto r = h2("/sse");
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.body, "event: greet\nid: 1\ndata: hello\ndata: world\n\n");
}

TEST_F(Http2ParityTest, ReadBodyStreamDeliversTheBody) {
    auto r = h2("/upload", "", std::string(3000, 'u'));
    ASSERT_EQ(r.code, CURLE_OK) << curl_easy_strerror(r.code);
    EXPECT_EQ(r.body, "got 3000");
}
