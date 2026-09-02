#include <orbit/server/App.hpp>
#include <iostream>

using namespace server;
using namespace http;

int main(int argc, char* argv[]) {
    auto config = config::ServerConfig::parse(argc, argv);
    config.worker_threads = std::thread::hardware_concurrency();
    
    App app(config);

    app.get("/", [](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().send("Hello, World!"));
    });

    app.get("/json", [](HttpRequest&, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().json(nlohmann::json{{"message", "Hello, World!"}}));
    });

    std::cout << "Starting Orbit Benchmark Server on port " << config.port 
              << " with " << config.worker_threads << " threads...\n";
    app.listen();

    return 0;
}
