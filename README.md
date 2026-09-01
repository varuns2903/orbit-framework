<div align="center">
  
  <h1>🚀 Orbit Framework</h1>
  
  <p><b>A blazing fast, asynchronous, and middleware-driven C++20 HTTP/3 web framework</b></p>
  
  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=c%2B%2B" />
    <img src="https://img.shields.io/badge/Engine-io__uring%20%7C%20epoll-orange.svg?style=for-the-badge&logo=linux" />
    <img src="https://img.shields.io/badge/Protocol-HTTP%2F3%20%7C%20QUIC-purple.svg?style=for-the-badge" />
    <img src="https://img.shields.io/badge/Status-Active-brightgreen.svg?style=for-the-badge" />
  </p>
</div>

---

Orbit brings modern web development ergonomics (like Express.js) to C++20, powered by raw kernel performance (`io_uring`/`epoll`) and modern protocols (HTTP/3 + QUIC).

## ✨ Features at a Glance

* **⚡ Blazing Fast**: Asynchronous kernel event loops with a highly scalable Proactor pattern.
* **🌐 Next-Gen Protocols**: Native support for HTTP/1.1, HTTP/2, and **HTTP/3 & QUIC**.
* **🛡️ Express-Style Middleware**: Routing, JWT Auth, CORS, Rate Limiting, and automated JSON validation built-in.
* **🔌 Real-Time**: Fully RFC-compliant WebSockets.
* **💾 Async Databases**: C++20 Coroutine-based PostgreSQL, Redis, and MongoDB clients natively integrated.
* **📦 Drop-in Ready**: Seamless support for `vcpkg`, `Conan`, and CMake `FetchContent` out of the box.

---

## 💻 Quick Start

Fetching Orbit into your own project is ridiculously easy using CMake:

```cmake
include(FetchContent)
FetchContent_Declare(
  OrbitFramework
  GIT_REPOSITORY https://github.com/varuns2903/orbit-framework.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(OrbitFramework)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```

## 📝 Code Example

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

    // 🚀 C++20 Coroutines (Non-Blocking async/await)
    app.get("/db", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) -> concurrency::Task {
        auto pg = std::make_shared<database::PostgresClient>(&res->proactor(), "dbname=postgres");
        if (co_await database::connect_async(pg)) {
            PGresult* db_res = co_await database::query_async(pg, "SELECT current_timestamp;");
            res->send(HttpResponse().status(HttpStatus::OK).send("DB Time: " + std::string(PQgetvalue(db_res, 0, 0))));
        }
    });

    app.listen(3000, []() { std::cout << "Listening on port 3000!" << std::endl; });
    return 0;
}
```

---

## 📖 Master Documentation

Dive deep into the architecture and learn how to master Orbit Framework:

* [🚀 **Getting Started**](docs/getting_started.md) - *Manual builds, vcpkg, and Conan instructions*
* [🛣️ **Routing & Streaming**](docs/routing.md)
* [🛡️ **Middleware & Validation**](docs/middleware.md)
* [💾 **Postgres & C++20 Coroutines**](docs/database.md)
* [🔀 **API Gateway & Load Balancing**](docs/proxy.md)
* [🔌 **WebSockets**](docs/websockets.md)
* [⚡ **HTTP/3 & QUIC**](docs/http3.md)

---

<div align="center">
  <br/>
  <i>Distributed under the MIT License. Built with ❤️ for the C++ Community.</i>
</div>
