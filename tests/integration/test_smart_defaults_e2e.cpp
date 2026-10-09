#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifdef __linux__
#include <sched.h>
#endif

// "auto" settings (#173): event loops and worker threads follow the CPUs the
// process may use, the engine is chosen at start-up, and loops can be pinned.

using namespace orbit::http;

namespace {

constexpr int kPort = 8173;

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

long get(const std::string& path, std::string* body) {
    CURL* curl = curl_easy_init();
    std::string url = "http://127.0.0.1:" + std::to_string(kPort) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return status;
}

size_t usable_cpus() {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) return static_cast<size_t>(CPU_COUNT(&set));
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

} // namespace

TEST(SmartDefaultsTest, AutoSettingsResolveAndServe) {
    orbit::config::ServerConfig cfg = orbit::test::server_config();
    cfg.host = "127.0.0.1";
    cfg.port = kPort;
    cfg.event_loops = 0;     // auto
    cfg.worker_threads = 0;  // auto
    // Auto picks io_uring on this kernel; do that only on io_uring runs
    // (ORBIT_TEST_ENGINE=iouring), not in the valgrind run, which uses epoll
    // and cannot follow io_uring's shared rings.
    const bool iouring_run = cfg.engine == orbit::config::EventEngine::IoUring;
    if (iouring_run) cfg.engine = orbit::config::EventEngine::Auto;
    cfg.cpu_affinity = true;
    orbit::server::App app(cfg);
    app.get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.set_body("ok", "text/plain");
        w->send(std::move(res));
    });

    std::thread server([&] { app.listen(); });
    std::string body;
    long status = 0;
    for (int i = 0; i < 100 && status != 200; ++i) {
        body.clear();
        status = get("/ok", &body);
        if (status != 200) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto effective = app.effective_config();
    const size_t loops = app.connections_per_event_loop().size();
    app.stop();
    server.join();

    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "ok");
    EXPECT_NE(effective.engine, orbit::config::EventEngine::Auto) << "the engine is decided at start-up";
    if (iouring_run) {
        EXPECT_EQ(effective.engine, orbit::config::EventEngine::IoUring) << "Linux 5.7+ has fast poll";
    }
    EXPECT_EQ(effective.worker_threads, usable_cpus());
#ifdef __linux__
    EXPECT_EQ(effective.event_loops, usable_cpus());
    EXPECT_EQ(loops, usable_cpus());
#else
    EXPECT_EQ(effective.event_loops, 1u) << "several event loops are Linux-only";
    EXPECT_EQ(loops, 1u);
#endif
}
