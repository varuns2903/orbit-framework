#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/StaticFiles.hpp>
#include "../utils/TestClient.hpp"
#include "../utils/TestConfig.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

// Middleware, streaming and static files over a real connection (#16): the
// unit tests call each middleware directly; these check what a client sees.

using namespace orbit::http;
using orbit::test::send_request;

namespace {

constexpr int kPort = 8170;

std::string url(const std::string& path) { return "http://127.0.0.1:" + std::to_string(kPort) + path; }

std::filesystem::path static_dir() {
    return std::filesystem::temp_directory_path() / "orbit_middleware_wire_static";
}

orbit::routing::RouteHandler reply(std::string body, std::string type = "text/plain") {
    return [body, type](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.set_body(body, type);
        w->send(std::move(res));
    };
}

#ifndef _WIN32
// A raw request, for paths a client library would normalise first.
std::string raw_request(const std::string& request) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return out;
}
#endif

} // namespace

class MiddlewareWireTest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        std::filesystem::create_directories(static_dir() / "sub");
        std::ofstream(static_dir() / "hello.txt") << "hello from disk";
        std::ofstream(static_dir() / "page.html") << "<p>hi</p>";
        std::ofstream(static_dir().parent_path() / "orbit_middleware_wire_secret.txt") << "secret";

        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<orbit::server::App>(cfg);

        app->group("/cors", [](orbit::routing::Router& r) {
            orbit::middleware::CorsOptions opts;
            opts.allowed_origins = {"https://app.example"};
            r.use(orbit::middleware::cors(opts));
            r.get("/data", reply("cors data"));
            r.options("/data", reply("")); // the preflight is answered by the middleware
        });
        app->group("/limited", [](orbit::routing::Router& r) {
            r.use(orbit::middleware::rate_limit(3, std::chrono::seconds(60)));
            r.get("/x", reply("ok"));
        });
        app->group("/csrf", [](orbit::routing::Router& r) {
            r.use(orbit::middleware::csrf_protection());
            r.get("/form", reply("form"));
            r.post("/submit", reply("accepted"));
        });
        app->group("/gz", [](orbit::routing::Router& r) {
            r.use(orbit::middleware::compress());
            r.get("/big", reply(std::string(4096, 'a')));
        });
        app->get("/chunked", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.headers["Content-Type"] = "text/plain";
            w->send_headers(res);
            w->write_chunk("one,");
            w->write_chunk("two,");
            w->write_chunk("three");
            w->end();
        });
        app->get("/events", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.headers["Content-Type"] = "text/event-stream";
            w->send_headers(res);
            w->send_sse_event("hello\nworld", "greet", "1");
            w->send_sse_event("bye");
            w->end();
        });
        // App-wide: requests that match no file continue to the routes above.
        app->use(orbit::middleware::static_files(static_dir().string()));

        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            if (send_request("GET", url("/hello.txt")).status_code > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
        std::error_code ec;
        std::filesystem::remove_all(static_dir(), ec);
        std::filesystem::remove(static_dir().parent_path() / "orbit_middleware_wire_secret.txt", ec);
    }
};

std::unique_ptr<orbit::server::App> MiddlewareWireTest::app;
std::thread MiddlewareWireTest::server_thread;

TEST_F(MiddlewareWireTest, CorsPreflightAndAllowList) {
    auto pre = send_request("OPTIONS", url("/cors/data"), "",
                            {"Origin: https://app.example", "Access-Control-Request-Method: GET"});
    EXPECT_EQ(pre.status_code, 204);
    EXPECT_EQ(pre.headers["Access-Control-Allow-Origin"], "https://app.example");

    auto allowed = send_request("GET", url("/cors/data"), "", {"Origin: https://app.example"});
    EXPECT_EQ(allowed.status_code, 200);
    EXPECT_EQ(allowed.headers["Access-Control-Allow-Origin"], "https://app.example");

    auto other = send_request("GET", url("/cors/data"), "", {"Origin: https://evil.example"});
    EXPECT_EQ(other.headers.count("Access-Control-Allow-Origin"), 0u) << "unlisted origins get no CORS headers";
}

TEST_F(MiddlewareWireTest, RateLimiterAnswers429WithRetryAfter) {
    for (int i = 0; i < 3; ++i) EXPECT_EQ(send_request("GET", url("/limited/x")).status_code, 200) << i;
    auto limited = send_request("GET", url("/limited/x"));
    EXPECT_EQ(limited.status_code, 429);
    EXPECT_FALSE(limited.headers["Retry-After"].empty());
}

TEST_F(MiddlewareWireTest, CsrfIssuesATokenAndChecksIt) {
    auto form = send_request("GET", url("/csrf/form"));
    EXPECT_EQ(form.status_code, 200);
    // The token cookie is issued on the first safe request.
    bool cookie_set = false;
    for (const auto& [k, v] : form.headers) {
        if (k == "Set-Cookie" && v.find("csrf_token=") != std::string::npos) cookie_set = true;
    }
    EXPECT_TRUE(cookie_set);

    EXPECT_EQ(send_request("POST", url("/csrf/submit"), "x=1").status_code, 403);
    auto ok = send_request("POST", url("/csrf/submit"), "x=1",
                           {"Cookie: csrf_token=t0k3n-value", "X-CSRF-Token: t0k3n-value"});
    EXPECT_EQ(ok.status_code, 200);
    EXPECT_EQ(ok.body, "accepted");
    auto wrong = send_request("POST", url("/csrf/submit"), "x=1",
                              {"Cookie: csrf_token=t0k3n-value", "X-CSRF-Token: other"});
    EXPECT_EQ(wrong.status_code, 403);
}

TEST_F(MiddlewareWireTest, CompressionIsNegotiated) {
    auto gz = send_request("GET", url("/gz/big"), "", {"Accept-Encoding: gzip"});
    EXPECT_EQ(gz.status_code, 200);
    EXPECT_EQ(gz.headers["Content-Encoding"], "gzip");
    EXPECT_LT(gz.body.size(), 4096u) << "the body went out compressed";

    auto plain = send_request("GET", url("/gz/big"));
    EXPECT_EQ(plain.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(plain.body.size(), 4096u);
}

TEST_F(MiddlewareWireTest, ChunkedResponseArrivesWhole) {
    auto r = send_request("GET", url("/chunked"));
    EXPECT_EQ(r.status_code, 200);
    EXPECT_EQ(r.headers["Transfer-Encoding"], "chunked");
    EXPECT_EQ(r.body, "one,two,three");
}

TEST_F(MiddlewareWireTest, ServerSentEvents) {
    auto r = send_request("GET", url("/events"));
    EXPECT_EQ(r.status_code, 200);
    EXPECT_EQ(r.headers["Content-Type"], "text/event-stream");
    EXPECT_EQ(r.body, "event: greet\nid: 1\ndata: hello\ndata: world\n\ndata: bye\n\n");
}

TEST_F(MiddlewareWireTest, StaticFilesTypesAndMissingFiles) {
    auto txt = send_request("GET", url("/hello.txt"));
    EXPECT_EQ(txt.status_code, 200);
    EXPECT_EQ(txt.body, "hello from disk");
    EXPECT_NE(txt.headers["Content-Type"].find("text/plain"), std::string::npos) << txt.headers["Content-Type"];

    auto html = send_request("GET", url("/page.html"));
    EXPECT_NE(html.headers["Content-Type"].find("text/html"), std::string::npos) << html.headers["Content-Type"];

    EXPECT_EQ(send_request("GET", url("/missing.txt")).status_code, 404);
}

#ifndef _WIN32
TEST_F(MiddlewareWireTest, StaticFilesRefuseToLeaveTheirDirectory) {
    for (const char* path : {"/../orbit_middleware_wire_secret.txt",
                             "/sub/../../orbit_middleware_wire_secret.txt",
                             "/%2e%2e/orbit_middleware_wire_secret.txt"}) {
        std::string res = raw_request(std::string("GET ") + path + " HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
        ASSERT_FALSE(res.empty()) << path;
        EXPECT_EQ(res.find("secret"), std::string::npos) << path << " leaked the file:\n" << res;
        EXPECT_EQ(res.find("HTTP/1.1 200"), std::string::npos) << path << "\n" << res;
    }
}
#endif
