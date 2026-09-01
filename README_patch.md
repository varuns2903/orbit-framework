## 📝 REST API & Coroutines

```cpp
#include "server/App.hpp"
#include "middleware/Validation.hpp"
#include "database/PostgresCoro.hpp"
#include "concurrency/Task.hpp"

using namespace server;
using namespace http;
using namespace middleware;

int main() {
    App app;

    // 🛡️ Global Error Handler
    app.on_error([](const std::exception& e, HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
        writer->send(HttpResponse().status(HttpStatus::InternalServerError).send("Crash prevented!"));
    });

    // ⚡ Automated JSON Validation
    std::vector<SchemaField> user_schema = {
        {"username", JsonType::STRING, true},
        {"age", JsonType::NUMBER, true}
    };

    app.post("/users", {validate_json(user_schema)}, [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        std::string username = req.json()["username"];
        res->json({{"status", "created"}, {"username", username}});
    });

    // 🚀 C++20 Coroutines (Non-Blocking async/await) & Unified DBAL
    app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) -> concurrency::Task {
        auto pg = std::make_shared<database::PostgresClient>(&res->proactor(), "dbname=postgres");
        if (co_await database::connect_async(pg)) {
            database::ResultSet db_res = co_await database::query_async(pg, "SELECT current_timestamp;");
            res->send(HttpResponse().status(HttpStatus::OK).send("DB Time: " + db_res[0].get(0).value_or("")));
        }
    });

    app.listen(3000, []() { std::cout << "Listening on port 3000!" << std::endl; });
    return 0;
}
```

## 🔌 Strongly-Typed WebSockets (Upcoming)

Orbit is pioneering a new way to write Real-Time applications in C++ by fusing **Strongly-Typed Event Routing** with **Session State**. No more manual JSON parsing or casting!

```cpp
// 1. Define your C++ structs
struct PlayerSession { std::string username; int score = 0; };
struct MoveEvent { int position; };

// 2. Tell the JSON parser about your structs
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MoveEvent, position)

int main() {
    App app;

    // 3. The framework handles session memory, routing, and JSON serialization automatically!
    app.ws<PlayerSession>("/game", [](EventSocket<PlayerSession>& ws) {
        
        ws.on("login", [&ws](const nlohmann::json& data) {
            ws.session().username = data["username"];
            ws.join("matchmaking");
        });

        // Automatically maps incoming `{"event": "move", "data": {"position": 4}}` to the MoveEvent struct
        ws.on<MoveEvent>("move", [&ws](const MoveEvent& event) {
            if (event.position == 4) {
                ws.session().score += 100;
                ws.to("matchmaking").emit("win", ws.session().username + " won!");
            }
        });
    });
}
```
