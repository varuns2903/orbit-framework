#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/json.hpp>
#include <iostream>
#include <string>

using namespace server;
using namespace http;

// 1. Define your standard C++ structs
struct UserProfile {
    std::string name;
    std::string role;
    int age;
};

struct ServerStatus {
    std::string status;
    int active_connections;
    double uptime_seconds;
};

// 2. Tell the JSON library how to serialize them
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UserProfile, name, role, age)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ServerStatus, status, active_connections, uptime_seconds)

int main() {
    config::ServerConfig config;
    config.port = 8083;
    App app(config);

    // MAGIC HANDLER 1: Returns a string automatically (Content-Type: text/plain)
    app.get("/hello", []() -> std::string {
        return "Hello, World! I was returned directly from the lambda.";
    });

    // MAGIC HANDLER 2: Returns a struct automatically (Content-Type: application/json)
    // Orbit's routing template detects the struct, uses nlohmann::json to serialize it,
    // constructs an HTTP 200 OK response, and sends it!
    app.get("/profile", []() -> UserProfile {
        return UserProfile{"Alice", "Admin", 28};
    });

    // MAGIC HANDLER 3: Accepts HttpRequest but still returns a struct
    app.get("/status", [](const HttpRequest& req) -> ServerStatus {
        std::cout << "Status requested by: " << req.headers.at("User-Agent") << "\n";
        return ServerStatus{"Healthy", 42, 3600.5};
    });

    // MAGIC HANDLER 4: Returning raw JSON objects works too!
    app.get("/raw_json", []() -> nlohmann::json {
        return {
            {"framework", "Orbit"},
            {"feature", "Magic Returns"},
            {"awesome", true}
        };
    });

    std::cout << "Starting FastAPI-style Server on port 8083...\n";
    std::cout << "Test 1 (Text): curl http://localhost:8083/hello\n";
    std::cout << "Test 2 (JSON Struct): curl http://localhost:8083/profile\n";
    std::cout << "Test 3 (JSON Struct with Req): curl -H \"User-Agent: curl/7.68.0\" http://localhost:8083/status\n";
    std::cout << "Test 4 (Raw JSON): curl http://localhost:8083/raw_json\n";
    
    app.listen();

    return 0;
}
