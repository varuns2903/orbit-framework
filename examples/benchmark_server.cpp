#include <orbit/server/App.hpp>
#include <iostream>
#include <string>
#include <thread>

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
