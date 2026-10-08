// The same two endpoints as examples/benchmark_server.cpp, on Crow.
// Usage: crow_bench <port> <threads>
#include "crow.h"
#include <cstdlib>

int main(int argc, char* argv[]) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8100;
    const int threads = argc > 2 ? std::atoi(argv[2]) : 1;

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    CROW_ROUTE(app, "/")([] {
        crow::response res("Hello, World!");
        res.set_header("Content-Type", "text/plain");
        return res;
    });
    CROW_ROUTE(app, "/json")([] {
        crow::json::wvalue body;
        body["message"] = "Hello, World!";
        return body;
    });
    app.bindaddr("127.0.0.1").port(static_cast<uint16_t>(port)).concurrency(static_cast<uint16_t>(threads)).run();
}
