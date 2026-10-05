#include <gtest/gtest.h>
#include <orbit/http/Client.hpp>
#include <orbit/server/App.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// http::Client against local Orbit servers (plain HTTP and TLS).

namespace {

constexpr uint16_t kPort = 8145;
constexpr uint16_t kTlsPort = 8146;
std::string g_cert, g_key;

std::string base() { return "http://127.0.0.1:" + std::to_string(kPort); }

bool make_self_signed(const std::string& cert_path, const std::string& key_path) {
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    X509* x509 = X509_new();
    if (!pkey || !x509) return false;
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 3600);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION* san = X509V3_EXT_conf_nid(nullptr, &ctx, NID_subject_alt_name, "IP:127.0.0.1");
    X509_add_ext(x509, san, -1);
    X509_EXTENSION_free(san);
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

network::socket_t connect_to(uint16_t port) {
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

class Server {
public:
    explicit Server(config::ServerConfig cfg) : port_(cfg.port), app_(std::make_unique<server::App>(cfg)) {
        app_->get("/hello", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.headers["X-Echo"] = std::string(req.headers["X-Test"]);
            res.set_body("hello " + req.query["name"]);
            w->send(std::move(res));
        });
        app_->post("/echo", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body(std::string(req.body));
            w->send(std::move(res));
        });
        app_->get("/missing", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.status(http::HttpStatus::NotFound);
            res.set_body("nope");
            w->send(std::move(res));
        });
        app_->get("/redirect/:n", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> w) {
            int n = std::stoi(req.params["n"]);
            http::HttpResponse res;
            if (n <= 0) {
                res.set_body("arrived");
            } else {
                res.status(http::HttpStatus::Found);
                res.headers["Location"] = "/redirect/" + std::to_string(n - 1);
            }
            w->send(std::move(res));
        });
        app_->get("/to-file", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.status(http::HttpStatus::Found);
            res.headers["Location"] = "file:///etc/passwd";
            w->send(std::move(res));
        });
        app_->get("/slow", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            http::HttpResponse res;
            res.set_body("late");
            w->send(std::move(res));
        });
        app_->get("/big", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body(std::string(200000, 'b'));
            w->send(std::move(res));
        });
        server::App* app = app_.get();
        thread_ = std::thread([app] { app->listen(); });
        for (int i = 0; i < 200; ++i) {
            network::socket_t fd = connect_to(port_);
            if (fd != network::INVALID_SOCKET_FD) {
                network::close_socket(fd);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~Server() {
        app_->stop();
        network::socket_t fd = connect_to(port_);
        if (fd != network::INVALID_SOCKET_FD) network::close_socket(fd);
        if (thread_.joinable()) thread_.join();
    }

private:
    uint16_t port_;
    std::unique_ptr<server::App> app_;
    std::thread thread_;
};

http::ClientRequest get(const std::string& path) {
    http::ClientRequest r;
    r.url = base() + path;
    return r;
}

// Collects send() completions for a test thread to wait on.
struct Latch {
    std::mutex m;
    std::condition_variable cv;
    size_t count = 0;
    void arrive() {
        std::lock_guard<std::mutex> lock(m);
        ++count;
        cv.notify_all();
    }
    bool wait_for(size_t n, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [&] { return count >= n; });
    }
};

concurrency::Task fetch_two(http::Client& client, std::string* out, Latch* done) {
    auto first_req = get("/hello?name=co");
    auto first = co_await client.send_async(first_req);
    auto second_req = get("/redirect/1");
    auto second = co_await client.send_async(second_req);
    *out = first.body + "|" + second.body;
    done->arrive();
}

} // namespace

class HttpClientTest : public ::testing::Test {
protected:
    static std::unique_ptr<Server> server;
    static std::unique_ptr<Server> tls_server;

    static void SetUpTestSuite() {
        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.worker_threads = 8;
        server = std::make_unique<Server>(cfg);

        auto dir = std::filesystem::temp_directory_path();
        g_cert = (dir / "orbit_client_cert.pem").string();
        g_key = (dir / "orbit_client_key.pem").string();
        if (make_self_signed(g_cert, g_key)) {
            config::ServerConfig tls;
            tls.port = kTlsPort;
            tls.ssl_cert = g_cert;
            tls.ssl_key = g_key;
            tls_server = std::make_unique<Server>(tls);
        }
    }
    static void TearDownTestSuite() {
        server.reset();
        tls_server.reset();
        std::filesystem::remove(g_cert);
        std::filesystem::remove(g_key);
    }
};

std::unique_ptr<Server> HttpClientTest::server;
std::unique_ptr<Server> HttpClientTest::tls_server;

TEST_F(HttpClientTest, GetWithHeadersAndQuery) {
    http::Client client;
    auto req = get("/hello?name=orbit");
    req.headers.push_back({"X-Test", "abc"});
    auto res = client.send_sync(req);
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, "hello orbit");
    EXPECT_EQ(res.header("x-echo"), "abc");
    EXPECT_EQ(res.header("CONTENT-LENGTH"), "11");
}

TEST_F(HttpClientTest, PostBody) {
    http::Client client;
    http::ClientRequest req = get("/echo");
    req.method = "POST";
    req.body = std::string(100000, 'p') + "end";
    auto res = client.send_sync(req);
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.body, req.body);
}

TEST_F(HttpClientTest, ErrorStatusIsAResponseNotAFailure) {
    http::Client client;
    auto res = client.send_sync(get("/missing"));
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.status, 404);
    EXPECT_EQ(res.body, "nope");
}

TEST_F(HttpClientTest, RedirectsAreFollowedWithinTheLimit) {
    http::Client client;
    auto res = client.send_sync(get("/redirect/3"));
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, "arrived");
    EXPECT_EQ(res.effective_url, base() + "/redirect/0");

    auto too_many = get("/redirect/9");
    too_many.max_redirects = 2;
    EXPECT_FALSE(client.send_sync(too_many).ok());

    auto manual = get("/redirect/1");
    manual.follow_redirects = false;
    auto first = client.send_sync(manual);
    ASSERT_TRUE(first.ok()) << first.error;
    EXPECT_EQ(first.status, 302);
    EXPECT_EQ(first.header("Location"), "/redirect/0");
}

TEST_F(HttpClientTest, RedirectToANonHttpUrlIsRefused) {
    http::Client client;
    auto res = client.send_sync(get("/to-file"));
    EXPECT_FALSE(res.ok());
    EXPECT_TRUE(res.body.empty());
}

TEST_F(HttpClientTest, OnlyHttpUrls) {
    http::Client client;
    http::ClientRequest req;
    req.url = "file:///etc/passwd";
    auto res = client.send_sync(req);
    EXPECT_FALSE(res.ok());
    EXPECT_NE(res.error.find("http"), std::string::npos) << res.error;
}

TEST_F(HttpClientTest, TimeoutAndSizeLimit) {
    http::Client client;
    auto slow = get("/slow");
    slow.timeout = std::chrono::milliseconds(300);
    auto started = std::chrono::steady_clock::now();
    auto res = client.send_sync(slow);
    auto took = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(res.ok());
    EXPECT_LT(took, std::chrono::milliseconds(1400));

    auto big = get("/big");
    big.max_response_size = 1000;
    auto limited = client.send_sync(big);
    EXPECT_FALSE(limited.ok());
    EXPECT_EQ(limited.error, "response larger than max_response_size");
    EXPECT_TRUE(limited.body.empty());
}

TEST_F(HttpClientTest, ConnectionRefused) {
    http::Client client;
    http::ClientRequest req;
    req.url = "http://127.0.0.1:1/";
    auto res = client.send_sync(req);
    EXPECT_FALSE(res.ok());
    EXPECT_EQ(res.status, 0);
}

TEST_F(HttpClientTest, ManyConcurrentRequests) {
    http::Client client;
    constexpr size_t kRequests = 100;
    Latch latch;
    std::atomic<size_t> good{0};
    for (size_t i = 0; i < kRequests; ++i) {
        client.send(get("/hello?name=" + std::to_string(i)), [&, i](http::ClientResponse res) {
            if (res.ok() && res.body == "hello " + std::to_string(i)) ++good;
            latch.arrive();
        });
    }
    ASSERT_TRUE(latch.wait_for(kRequests, std::chrono::seconds(30)));
    EXPECT_EQ(good.load(), kRequests);
}

TEST_F(HttpClientTest, CoroutinesAwaitResponses) {
    http::Client client;
    std::string out;
    Latch done;
    fetch_two(client, &out, &done);
    ASSERT_TRUE(done.wait_for(1, std::chrono::seconds(10)));
    EXPECT_EQ(out, "hello co|arrived");
}

TEST_F(HttpClientTest, SendSyncInsideACallbackFailsInsteadOfHanging) {
    http::Client client;
    Latch done;
    std::string error;
    client.send(get("/hello"), [&](http::ClientResponse) {
        error = client.send_sync(get("/hello")).error;
        done.arrive();
    });
    ASSERT_TRUE(done.wait_for(1, std::chrono::seconds(10)));
    EXPECT_NE(error.find("own thread"), std::string::npos) << error;
}

TEST_F(HttpClientTest, DestroyingTheClientCompletesPendingRequests) {
    Latch done;
    std::string error;
    {
        http::Client client;
        auto slow = get("/slow");
        client.send(slow, [&](http::ClientResponse res) {
            error = res.error;
            done.arrive();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(done.wait_for(1, std::chrono::seconds(1)));
    EXPECT_EQ(error, "client shut down");
}

TEST_F(HttpClientTest, TlsIsVerifiedUnlessTurnedOff) {
    ASSERT_TRUE(tls_server);
    http::Client client;
    http::ClientRequest req;
    req.url = "https://127.0.0.1:" + std::to_string(kTlsPort) + "/hello?name=tls";

    auto refused = client.send_sync(req); // self-signed: not trusted by default
    EXPECT_FALSE(refused.ok());

    auto trusted = req;
    trusted.ca_file = g_cert;
    auto with_ca = client.send_sync(trusted);
    ASSERT_TRUE(with_ca.ok()) << with_ca.error;
    EXPECT_EQ(with_ca.body, "hello tls");

    auto insecure = req;
    insecure.verify_tls = false;
    auto unverified = client.send_sync(insecure);
    ASSERT_TRUE(unverified.ok()) << unverified.error;
    EXPECT_EQ(unverified.body, "hello tls");
}

TEST_F(HttpClientTest, SharedClient) {
    auto res = http::Client::shared().send_sync(get("/hello?name=shared"));
    ASSERT_TRUE(res.ok()) << res.error;
    EXPECT_EQ(res.body, "hello shared");
}
