#include <orbit/server/App.hpp>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>

#ifdef ORBIT_BENCH_COUNT_ALLOCATIONS
// Every heap allocation in the process, for benchmarks/probe.sh: the probe
// reads /__stats before and after a load window and divides by the requests.
#include <cstdlib>
#include <new>

namespace {
std::atomic<unsigned long long> g_allocations{0};
std::atomic<unsigned long long> g_requests{0};
} // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace orbit::server;
using namespace orbit::http;

int main(int argc, char* argv[]) {
    auto config = orbit::config::ServerConfig::parse(argc, argv);
    // One worker per core unless --threads/-t says otherwise (the comparison
    // in benchmarks/run_benchmarks.sh gives every framework the same count).
    bool threads_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--threads" || arg == "-t") threads_given = true;
    }
    if (!threads_given) config.worker_threads = std::thread::hardware_concurrency();
    
    App app(config);

#ifdef ORBIT_BENCH_COUNT_ALLOCATIONS
    // Counts the requests the probe sends; /__stats itself is not counted.
    app.use([](HttpRequest& req, std::shared_ptr<ResponseWriter>) {
        if (req.uri != "/__stats") g_requests.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
    app.get("/__stats", [](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().json(nlohmann::json{
            {"allocations", g_allocations.load(std::memory_order_relaxed)},
            {"requests", g_requests.load(std::memory_order_relaxed)}}));
    });
#endif

    app.get("/", [](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().send("Hello, World!"));
    });

    app.get("/json", [](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().json(nlohmann::json{{"message", "Hello, World!"}}));
    });

    std::cout << "Starting Orbit Benchmark Server on port " << config.port
              << " with " << config.worker_threads << " worker threads and "
              << config.event_loops << " event loop(s)...\n";
    app.listen();

    return 0;
}
