#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <curl/curl.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// An on_error handler that itself throws: the client still gets a 500,
// promptly, on a real connection. Requests share one keep-alive connection,
// so "a response was already sent" must be tracked per request.

using namespace http;

namespace {

constexpr int kPort = 8153;

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

struct Result {
    long status = 0;
    std::string body;
    long new_connections = -1;
    CURLcode code = CURLE_OK;
};

// One request on `curl`, which keeps its connection between calls.
Result get(CURL* curl, const std::string& path) {
    Result r;
    std::string url = "http://127.0.0.1:" + std::to_string(kPort) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    r.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &r.new_connections);
    return r;
}

} // namespace

class ErrorHandlerE2ETest : public ::testing::Test {
protected:
    static std::unique_ptr<server::App> app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<server::App>(cfg);

        app->on_error([](const std::exception& e, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            if (std::string(e.what()) == "handled") {
                HttpResponse res;
                res.status(HttpStatus::BadRequest).set_body("handled", "text/plain");
                w->send(std::move(res));
                return;
            }
            throw std::runtime_error("the error handler failed as well");
        });
        app->get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("ok", "text/plain");
            w->send(std::move(res));
        });
        app->get("/handled", [](HttpRequest&, std::shared_ptr<ResponseWriter>) {
            throw std::runtime_error("handled");
        });
        app->get("/unhandled", [](HttpRequest&, std::shared_ptr<ResponseWriter>) {
            throw std::runtime_error("not handled");
        });

        server_thread = std::thread([] { app->listen(); });
        CURL* probe = curl_easy_init();
        for (int i = 0; i < 100 && get(probe, "/ok").status != 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        curl_easy_cleanup(probe);
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
    }
};

std::unique_ptr<server::App> ErrorHandlerE2ETest::app;
std::thread ErrorHandlerE2ETest::server_thread;

TEST_F(ErrorHandlerE2ETest, FailingErrorHandlerStillAnswersOnAKeepAliveConnection) {
    CURL* curl = curl_easy_init();
    ASSERT_NE(curl, nullptr);

    // A normal response first, so this connection has already answered once.
    Result ok = get(curl, "/ok");
    ASSERT_EQ(ok.code, CURLE_OK);
    EXPECT_EQ(ok.status, 200);

    auto start = std::chrono::steady_clock::now();
    Result failed = get(curl, "/unhandled");
    auto took = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(failed.code, CURLE_OK) << curl_easy_strerror(failed.code);
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(failed.body, "500 Internal Server Error");
    EXPECT_EQ(failed.new_connections, 0) << "the keep-alive connection was reused";
    EXPECT_LT(took, std::chrono::seconds(2)) << "answered at once, not after a timeout";

    // The connection still works afterwards, for handled errors too.
    Result handled = get(curl, "/handled");
    EXPECT_EQ(handled.status, 400);
    EXPECT_EQ(handled.body, "handled");
    Result again = get(curl, "/unhandled");
    EXPECT_EQ(again.status, 500);
    EXPECT_EQ(get(curl, "/ok").status, 200);

    curl_easy_cleanup(curl);
}
