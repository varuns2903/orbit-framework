#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/SecurityHeaders.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

namespace {

constexpr uint16_t kPort = 8117;
constexpr const char* kStrictCsp = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
                                   "img-src 'self' data:";

std::string get(const std::string& path) {
    orbit::network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        orbit::network::close_socket(fd);
        return "";
    }
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), static_cast<int>(req.size()), 0);
    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    orbit::network::close_socket(fd);
    return out;
}

} // namespace

class OpenApiDocsTest : public ::testing::Test {
protected:
    static orbit::server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kPort;
        app = new orbit::server::App(cfg);
        app->get("/ping", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
            orbit::http::HttpResponse res;
            res.set_body("pong");
            w->send(std::move(res));
        });
        app->enable_openapi("Test API", "1.0.0", "/docs", "/swagger.json");
        // Self-hosted assets, and a json path that tries to break out of the script.
        app->enable_openapi("Test API", "1.0.0", "/docs-local", "/spec.json?x='</script><b>",
                            "/swagger-ui/");
        // Self-hosted assets behind security_headers() with a strict policy.
        orbit::middleware::SecurityHeadersOptions headers;
        headers.content_security_policy = kStrictCsp;
        app->use("/docs-csp", orbit::middleware::security_headers(headers));
        app->enable_openapi("Test API", "1.0.0", "/docs-csp", "/spec-csp.json", "/swagger-ui");
        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 100 && get("/ping").find("pong") == std::string::npos; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        get("/ping");
        if (server_thread.joinable()) server_thread.join();
        delete app;
    }
};

orbit::server::App* OpenApiDocsTest::app = nullptr;
std::thread OpenApiDocsTest::server_thread;

TEST_F(OpenApiDocsTest, CdnAssetsArePinnedWithSubresourceIntegrity) {
    std::string page = get("/docs");
    ASSERT_EQ(page.rfind("HTTP/1.1 200", 0), 0u) << page;
    EXPECT_NE(page.find("href=\"https://unpkg.com/swagger-ui-dist@5.11.0/swagger-ui.css\""), std::string::npos);
    EXPECT_NE(page.find("src=\"https://unpkg.com/swagger-ui-dist@5.11.0/swagger-ui-bundle.js\""), std::string::npos);
    EXPECT_NE(page.find("integrity=\"sha384-+yyzNgM3K92sROwsXxYCxaiLWxWJ0G+v/9A+qIZ2rgefKgkdcmJI+L601cqPD/Ut\""),
              std::string::npos);
    EXPECT_NE(page.find("integrity=\"sha384-qn5tagrAjZi8cSmvZ+k3zk4+eDEEUcP9myuR2J6V+/H6rne++v6ChO7EeHAEzqxQ\""),
              std::string::npos);
    EXPECT_NE(page.find("crossorigin=\"anonymous\""), std::string::npos);
    EXPECT_NE(page.find("<script src=\"/docs/init.js\"></script>"), std::string::npos) << page;
}

// Every script on the page is an external file, so a CSP of
// "script-src 'self' https://unpkg.com" (no 'unsafe-inline') still runs
// Swagger UI (#192).
TEST_F(OpenApiDocsTest, PageHasNoInlineScript) {
    for (const char* path : {"/docs", "/docs-local", "/docs-csp"}) {
        std::string page = get(path);
        ASSERT_EQ(page.rfind("HTTP/1.1 200", 0), 0u) << path << "\n" << page;
        size_t scripts = 0;
        for (size_t at = page.find("<script"); at != std::string::npos; at = page.find("<script", at + 1)) {
            ++scripts;
            EXPECT_EQ(page.compare(at, 12, "<script src="), 0) << path << ": inline script at " << at;
            size_t close = page.find("</script>", at);
            ASSERT_NE(close, std::string::npos);
            EXPECT_EQ(page[close - 1], '>') << path << ": script with a body";
        }
        EXPECT_EQ(scripts, 2u) << path;
    }
}

TEST_F(OpenApiDocsTest, InitScriptStartsSwaggerUi) {
    std::string js = get("/docs/init.js");
    ASSERT_EQ(js.rfind("HTTP/1.1 200", 0), 0u) << js;
    EXPECT_NE(js.find("Content-Type: text/javascript"), std::string::npos) << js;
    EXPECT_NE(js.find("SwaggerUIBundle({"), std::string::npos) << js;
    EXPECT_NE(js.find("url: '/swagger.json'"), std::string::npos) << js;
}

// With security_headers() and a strict policy, the page and its init script
// are served under that policy, same-origin.
TEST_F(OpenApiDocsTest, WorksUnderAStrictContentSecurityPolicy) {
    std::string page = get("/docs-csp");
    EXPECT_NE(page.find("Content-Security-Policy: " + std::string(kStrictCsp)), std::string::npos) << page;
    EXPECT_NE(page.find("src=\"/swagger-ui/swagger-ui-bundle.js\""), std::string::npos) << page;
    EXPECT_NE(page.find("src=\"/docs-csp/init.js\""), std::string::npos) << page;
    std::string js = get("/docs-csp/init.js");
    EXPECT_EQ(js.rfind("HTTP/1.1 200", 0), 0u) << js;
    EXPECT_NE(js.find("url: '/spec-csp.json'"), std::string::npos) << js;
}

TEST_F(OpenApiDocsTest, AssetsCanBeSelfHosted) {
    std::string page = get("/docs-local");
    ASSERT_EQ(page.rfind("HTTP/1.1 200", 0), 0u) << page;
    EXPECT_NE(page.find("href=\"/swagger-ui/swagger-ui.css\""), std::string::npos) << page;
    EXPECT_NE(page.find("src=\"/swagger-ui/swagger-ui-bundle.js\""), std::string::npos) << page;
    EXPECT_EQ(page.find("unpkg.com"), std::string::npos);
}

TEST_F(OpenApiDocsTest, JsonPathCannotBreakOutOfTheScript) {
    std::string page = get("/docs-local");
    EXPECT_EQ(page.find("</script><b>"), std::string::npos);
    std::string js = get("/docs-local/init.js");
    EXPECT_EQ(js.find("</script><b>"), std::string::npos);
    EXPECT_NE(js.find("url: '/spec.json?x=\\'\\x3c/script>\\x3cb>'"), std::string::npos) << js;
}

TEST_F(OpenApiDocsTest, SpecIsServed) {
    std::string spec = get("/swagger.json");
    EXPECT_EQ(spec.rfind("HTTP/1.1 200", 0), 0u) << spec;
    EXPECT_NE(spec.find("Test API"), std::string::npos);
}
