#include <gtest/gtest.h>
#if defined(__linux__)
#include <orbit/server/App.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <curl/curl.h>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// io_uring requests submitted from a thread that then exits used to be
// cancelled by the kernel, so a response written from a short-lived thread
// was sometimes never delivered (#220). The io_uring proactor now submits
// only from its loop thread; other threads queue their requests (#169).

using namespace orbit::http;

namespace {

constexpr int kPort = 8172;

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

long get(CURL* curl, const std::string& path, std::string* body) {
    std::string url = "http://127.0.0.1:" + std::to_string(kPort) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return status;
}

} // namespace

TEST(IoUringSubmissionTest, ResponsesFromThreadsThatExitAreDelivered) {
    orbit::config::ServerConfig cfg = orbit::test::server_config();
    cfg.host = "127.0.0.1";
    cfg.port = kPort;
    cfg.engine = orbit::config::EventEngine::IoUring;
    orbit::server::App app(cfg);
    app.get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.set_body("ok", "text/plain");
        w->send(std::move(res));
    });
    // The response is written by a thread that exits right afterwards.
    app.get("/from-thread", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        std::thread([w] {
            HttpResponse res;
            res.set_body("from a short-lived thread", "text/plain");
            w->send(std::move(res));
        }).detach();
    });

    std::thread server([&] { app.listen(); });
    CURL* curl = curl_easy_init();
    std::string probe;
    for (int i = 0; i < 100 && get(curl, "/ok", &probe) != 200; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    int delivered = 0;
    constexpr int kRequests = 60;
    for (int i = 0; i < kRequests; ++i) {
        std::string body;
        if (get(curl, "/from-thread", &body) == 200 && body == "from a short-lived thread") ++delivered;
    }
    curl_easy_cleanup(curl);
    app.stop();
    server.join();

    EXPECT_EQ(delivered, kRequests) << "responses written from exiting threads were lost";
}

#endif
