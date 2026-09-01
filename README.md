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
#include <orbit/server/App.hpp>
#include <orbit/orm/Model.hpp>
#include <orbit/http/json.hpp>

using namespace server;
using namespace http;

// 1. Define standard C++ structs
struct User {
    int id;
    std::string username;
    int age;
};

// 2. Tell the JSON library how to serialize them automatically!
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(User, id, username, age)

// 3. Register as a strongly-typed Database ORM Model
ORBIT_REGISTER_MODEL(User, "users")

int main() {
    config::ServerConfig cfg;
    cfg.port = 3000;
    App app(cfg);

    // ✨ Magic Returns: Just return a struct! Orbit handles 200 OK and JSON serialization.
    app.get("/profile", []() -> User {
        return User{1, "Alice", 28};
    });

    // ✨ Returns strings automatically with 'text/plain'
    app.get("/hello", []() -> std::string {
        return "Hello from Orbit!";
    });

    // 🚀 Non-Blocking C++20 Coroutines & Expression Template ORM
    app.get("/active_users", [](HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
        auto coro = [writer]() -> concurrency::Task {
            auto db = std::make_shared<database::PostgresClient>(&writer->proactor(), "dbname=postgres");
            co_await connect_async(db);

            // Fully type-safe C++ DSL compiled into SQL!
            std::vector<User> adults = co_await query_User(db)
                .where(orm::Col("age") >= 18)
                .get_async();

            // Send vector (automatically serialized as JSON array)
            nlohmann::json j = adults;
            writer->send(HttpResponse().status(200).send(j.dump()));
        };
        coro();
    });

    app.listen();
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
