// The same two endpoints as examples/benchmark_server.cpp, on Drogon.
// Usage: drogon_bench <port> <threads>
#include <drogon/drogon.h>
#include <cstdlib>

int main(int argc, char* argv[]) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8100;
    const int threads = argc > 2 ? std::atoi(argv[2]) : 1;

    drogon::app().registerHandler("/", [](const drogon::HttpRequestPtr&,
                                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto res = drogon::HttpResponse::newHttpResponse();
        res->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        res->setBody("Hello, World!");
        callback(res);
    });
    drogon::app().registerHandler("/json", [](const drogon::HttpRequestPtr&,
                                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        Json::Value body;
        body["message"] = "Hello, World!";
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
    });
    drogon::app()
        .setLogLevel(trantor::Logger::kWarn)
        .addListener("127.0.0.1", static_cast<uint16_t>(port))
        .setThreadNum(static_cast<size_t>(threads))
        .run();
}
