#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// HTTP/3 requests through the Router, driven by libcurl. Skipped when the
// framework is built without HTTP/3 or libcurl cannot speak it.

#ifdef ORBIT_ENABLE_HTTP3

namespace {

constexpr uint16_t kPort = 8140;
constexpr size_t kMaxBody = 2 * 1024 * 1024;
std::string g_cert, g_key, g_file;

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

int trace(CURL*, curl_infotype type, char* data, size_t size, void* out) {
    if (type == CURLINFO_TEXT || type == CURLINFO_HEADER_IN || type == CURLINFO_HEADER_OUT) {
        auto* log = static_cast<std::string*>(out);
        const char* tag = type == CURLINFO_TEXT ? "* " : type == CURLINFO_HEADER_IN ? "< " : "> ";
        log->append(tag);
        log->append(data, size);
    }
    return 0;
}

struct Result {
    CURLcode code;
    long status;
    long version;
    std::string headers;
    std::string body;
    std::string trace;
};

std::string describe(const Result& r) {
    return std::string(curl_easy_strerror(r.code)) + "\n--- curl trace ---\n" + r.trace;
}

struct Request {
    std::string path;
    std::string cookie;
    std::string post;
    bool head = false;
    long timeout_ms = 10000;
};

Result h3(const Request& req) {
    Result r{};
    CURL* c = curl_easy_init();
    std::string url = "https://127.0.0.1:" + std::to_string(kPort) + req.path;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, req.timeout_ms);
    curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, trace);
    curl_easy_setopt(c, CURLOPT_DEBUGDATA, &r.trace);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
    if (!req.cookie.empty()) curl_easy_setopt(c, CURLOPT_COOKIE, req.cookie.c_str());
    if (req.head) curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    if (!req.post.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.post.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.post.size()));
    }
    r.code = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &r.version);
    curl_easy_cleanup(c);
    return r;
}

Result h3(const std::string& path) {
    Request req;
    req.path = path;
    return h3(req);
}

bool curl_has_http3() {
    return (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTP3) != 0;
}

// Wakes the event loop so a stop request is seen at once.
void poke_server() {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    network::close_socket(fd);
}

std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
    return s;
}

} // namespace

class Http3Test : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;
    // Set when the server could not be started. Reported by each test:
    // gtest turns a SetUpTestSuite failure into skipped tests, which ctest
    // then counts as skipped rather than failed.
    static std::string setup_error;

    static void SetUpTestSuite() {
        if (!curl_has_http3()) return;
        auto dir = std::filesystem::temp_directory_path();
        g_cert = (dir / "orbit_h3_cert.pem").string();
        g_key = (dir / "orbit_h3_key.pem").string();
        g_file = (dir / "orbit_h3_file.bin").string();
        if (!make_self_signed(g_cert, g_key)) {
            setup_error = "could not create a test certificate";
            return;
        }
        {
            std::ofstream f(g_file, std::ios::binary);
            std::string data = pattern(3 * 1024 * 1024 + 123);
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
        }

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.ssl_cert = g_cert;
        cfg.ssl_key = g_key;
        cfg.http_version = config::HttpVersion::Http3;
        cfg.max_body_size = kMaxBody;
        app = new server::App(cfg);

        app->use([](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            w->add_interceptor([](http::HttpResponse& res) { res.headers["X-Intercepted"] = "yes"; });
            w->set_header("X-Default", "d");
            return true;
        });
        app->get("/whoami", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("uri=" + req.uri + " q=" + req.query["q"] + " session=" + req.cookies["session"] +
                         " theme=" + req.cookies["theme"] + " host=" + std::string(req.headers["Host"]) +
                         " ip=" + req.client_ip + " v=" + req.http_version);
            w->send(std::move(res));
        });
        app->get("/users/:id", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("user " + req.params["id"]);
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
        app->get("/file", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.send_file(g_file, "application/octet-stream");
            w->send(std::move(res));
        });
        app->get("/blob", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body(pattern(256 * 1024));
            w->send(std::move(res));
        });
        app->get("/empty", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.status(http::HttpStatus::NoContent);
            w->send(std::move(res));
        });
        app->post("/upload", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            std::string body(req.body);
            http::HttpResponse res;
            res.set_body("got " + std::to_string(body.size()) + " ok=" + (body == pattern(body.size()) ? "1" : "0"));
            w->send(std::move(res));
        });

        server_thread = std::thread([] { app->listen(); });
        Result probe{};
        for (int i = 0; i < 50; ++i) {
            Request req;
            req.path = "/whoami";
            req.timeout_ms = 1000;
            probe = h3(req);
            if (probe.code == CURLE_OK && probe.status == 200) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        setup_error = "server never answered over HTTP/3: " + describe(probe);
    }

    void SetUp() override {
        if (curl_has_http3()) {
            if (!setup_error.empty()) FAIL() << setup_error;
            return;
        }
        // CI sets this so a libcurl without HTTP/3 fails loudly instead of
        // skipping the whole suite unnoticed.
        if (std::getenv("ORBIT_REQUIRE_HTTP3_TESTS")) FAIL() << "libcurl was built without HTTP/3 support";
        GTEST_SKIP() << "libcurl was built without HTTP/3 support";
    }

    static void TearDownTestSuite() {
        if (!app) return;
        app->stop();
        poke_server();
        if (server_thread.joinable()) server_thread.join();
        delete app;
        app = nullptr;
        std::filesystem::remove(g_cert);
        std::filesystem::remove(g_key);
        std::filesystem::remove(g_file);
    }
};

server::App* Http3Test::app = nullptr;
std::thread Http3Test::server_thread;
std::string Http3Test::setup_error;

TEST_F(Http3Test, RoutesQueryCookiesAndHooks) {
    Request req;
    req.path = "/whoami?q=hello%20there&x=1";
    req.cookie = "session=abc; theme=dark";
    auto r = h3(req);
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.version, static_cast<long>(CURL_HTTP_VERSION_3));
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "uri=/whoami q=hello there session=abc theme=dark host=127.0.0.1:8140 ip=127.0.0.1 v=HTTP/3");
    EXPECT_NE(r.headers.find("x-intercepted: yes"), std::string::npos) << r.headers;
    EXPECT_NE(r.headers.find("x-default: d"), std::string::npos) << r.headers;
}

TEST_F(Http3Test, PathParametersAndNotFound) {
    auto r = h3("/users/42");
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.body, "user 42");
    auto missing = h3("/no/such/route");
    ASSERT_EQ(missing.code, CURLE_OK) << describe(missing);
    EXPECT_EQ(missing.status, 404);
}

TEST_F(Http3Test, MalformedPathIsRejected) {
    auto r = h3("/users/a%2Fb");
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 400);
}

TEST_F(Http3Test, WriteChunkStreamsTheBody) {
    auto r = h3("/stream");
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "one,two,three");
}

TEST_F(Http3Test, ServerSentEvents) {
    auto r = h3("/sse");
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.body, "event: greet\nid: 1\ndata: hello\ndata: world\n\n");
}

TEST_F(Http3Test, FileResponseIsSentWhole) {
    auto r = h3("/file");
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 200);
    ASSERT_EQ(r.body.size(), 3u * 1024 * 1024 + 123);
    EXPECT_TRUE(r.body == pattern(r.body.size()));
}

TEST_F(Http3Test, HeadAndNoContentHaveNoBody) {
    Request head;
    head.path = "/whoami";
    head.head = true;
    auto r = h3(head);
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body.empty());

    auto empty = h3("/empty");
    ASSERT_EQ(empty.code, CURLE_OK) << describe(empty);
    EXPECT_EQ(empty.status, 204);
    EXPECT_TRUE(empty.body.empty());
}

TEST_F(Http3Test, UploadsReachTheHandlerIntact) {
    Request req;
    req.path = "/upload";
    req.post = pattern(1024 * 1024 + 7);
    auto r = h3(req);
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, "got 1048583 ok=1");
}

TEST_F(Http3Test, OversizedBodyGets413) {
    Request req;
    req.path = "/upload";
    req.post = pattern(kMaxBody + 1);
    auto r = h3(req);
    ASSERT_EQ(r.code, CURLE_OK) << describe(r);
    EXPECT_EQ(r.status, 413);
}

TEST_F(Http3Test, ManySequentialRequests) {
    for (int i = 0; i < 20; ++i) {
        auto r = h3("/users/" + std::to_string(i));
        ASSERT_EQ(r.code, CURLE_OK) << "request " << i << ": " << describe(r);
        EXPECT_EQ(r.body, "user " + std::to_string(i));
    }
}

TEST_F(Http3Test, ConcurrentStreamsOnOneConnection) {
    // Handlers finish on different worker threads while the event loop
    // keeps reading: responses are submitted concurrently on one connection.
    // Kept small enough for the valgrind run in CI.
    constexpr int kStreams = 16;
    CURLM* multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 1L);
    std::vector<CURL*> handles;
    std::vector<std::string> bodies(kStreams);
    std::vector<std::string> urls;
    for (int i = 0; i < kStreams; ++i) {
        urls.push_back("https://127.0.0.1:" + std::to_string(kPort) + (i % 2 ? "/blob" : "/users/" + std::to_string(i)));
    }
    for (int i = 0; i < kStreams; ++i) {
        CURL* c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, urls[i].c_str());
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_3ONLY);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 20000L);
        curl_easy_setopt(c, CURLOPT_PIPEWAIT, 1L); // wait for the first connection and share it
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &bodies[i]);
        curl_multi_add_handle(multi, c);
        handles.push_back(c);
    }
    int running = 0;
    do {
        curl_multi_perform(multi, &running);
        if (running) curl_multi_poll(multi, nullptr, 0, 100, nullptr);
    } while (running);

    long connects = 0;
    for (int i = 0; i < kStreams; ++i) {
        long status = 0, n = 0;
        curl_easy_getinfo(handles[i], CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(handles[i], CURLINFO_NUM_CONNECTS, &n);
        connects += n;
        EXPECT_EQ(status, 200) << "stream " << i;
        if (i % 2) {
            EXPECT_EQ(bodies[i].size(), 256u * 1024) << "stream " << i;
            EXPECT_TRUE(bodies[i] == pattern(bodies[i].size())) << "stream " << i;
        } else {
            EXPECT_EQ(bodies[i], "user " + std::to_string(i));
        }
        curl_multi_remove_handle(multi, handles[i]);
        curl_easy_cleanup(handles[i]);
    }
    curl_multi_cleanup(multi);
    EXPECT_EQ(connects, 1) << "the requests did not share one connection";
}

#else

TEST(Http3Test, DisabledInThisBuild) {
    GTEST_SKIP() << "built with ORBIT_ENABLE_HTTP3=OFF";
}

#endif
