<div align="center">
  
  <h1>🚀 Orbit Framework</h1>
  
  <p><b>A blazing fast, asynchronous C++20 web framework with HTTP/3, WebSockets, and a built-in ORM</b></p>
  
  <p>
    <a href="https://github.com/varuns2903/orbit-framework/actions"><img src="https://img.shields.io/github/actions/workflow/status/varuns2903/orbit-framework/ci.yml?style=for-the-badge&label=CI&logo=github" /></a>
    <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=c%2B%2B" />
    <img src="https://img.shields.io/badge/Protocols-HTTP%2F1.1%20%7C%20HTTP%2F2%20%7C%20HTTP%2F3-purple.svg?style=for-the-badge" />
    <a href="https://github.com/varuns2903/orbit-framework/releases"><img src="https://img.shields.io/github/v/release/varuns2903/orbit-framework?style=for-the-badge&logo=github&label=Release" /></a>
    <img src="https://img.shields.io/badge/Platforms-Linux%20%7C%20macOS%20%7C%20Windows-brightgreen.svg?style=for-the-badge" />
  </p>
</div>

---

Orbit brings **Express.js ergonomics** to C++20, powered by raw kernel performance (`io_uring` / `epoll` / `kqueue` / `IOCP`) and next-gen protocols (HTTP/3 + QUIC). Write async web servers, REST APIs, and real-time apps — without sacrificing the speed of C++.

## ⚡ Why Orbit?

| Feature | Details |
|---------|---------|
| **Blazing Fast** | Asynchronous Proactor pattern with `io_uring`, `epoll`, `kqueue`, and Windows IOCP |
| **Modern Protocols** | HTTP/1.1, HTTP/2, **HTTP/3 & QUIC** — no external proxy needed |
| **Express-Style API** | Routing, middleware chains, route groups, and dynamic parameters |
| **Magic Returns** | Return `std::string`, structs, or `nlohmann::json` from handlers — Orbit auto-serializes |
| **Built-in ORM** | Expression Template DSL: `Col("age") >= 18` compiles to SQL at zero runtime cost |
| **Real-Time** | RFC 6455 WebSockets + Socket.IO-style EventRouter with rooms & sessions |
| **13+ Middlewares** | CORS, JWT Auth, Rate Limiting, CSRF, Compression, Proxy, OAuth2, and more |
| **4 Database Clients** | PostgreSQL, MySQL/MariaDB, MongoDB, Redis — all async with C++20 coroutines |
| **Cross-Platform CI** | Tested on Ubuntu, macOS, and Windows with Valgrind leak detection |

---

## 🚀 Quick Start

### 1. Install (One-Command)

The fastest way to install Orbit and its CLI globally is via our installer scripts.

**For Linux and macOS:**
```bash
curl -sL https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.sh | bash
```

**For Windows (Run in PowerShell as Administrator):**
```powershell
iwr -useb https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.ps1 | iex
```

This will automatically download the framework, configure `vcpkg`, compile the core library in Release mode, and install the `orbit` CLI to your system path.

### 2. Create a Server

Create `main.cpp`:

```cpp
#include <orbit/server/App.hpp>

int main() {
    server::App app;

    // Return a string — Orbit handles the HTTP response automatically
    app.get("/", []() -> std::string {
        return "Hello from Orbit! 🚀";
    });

    // Return JSON from a struct
    app.get("/api/status", []() -> nlohmann::json {
        return {{"status", "ok"}, {"version", "1.4.0"}};
    });

    // Dynamic route parameters
    app.get("/users/:id", [](http::HttpRequest& req) -> nlohmann::json {
        return {{"user_id", req.params["id"]}};
    });

    app.listen();  // Default port: 8080
}
```

### 3. Build & Run

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
./build/basic_server
```

```bash
$ curl http://localhost:8080/
Hello from Orbit! 🚀

$ curl http://localhost:8080/api/status
{"status":"ok","version":"1.4.0"}

$ curl http://localhost:8080/users/42
{"user_id":"42"}
```

---

## 📝 Features in Action

### Middleware & Authentication

```cpp
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/RateLimiter.hpp>

app.use(middleware::cors());                                        // Global CORS
app.use(middleware::rate_limit(1000, std::chrono::seconds(60)));    // Rate limit

// Protected route group
app.group("/api/v1", [](routing::Router& r) {
    r.use(middleware::jwt_auth("your-secret-key"));
    r.get("/profile", [](http::HttpRequest& req) -> nlohmann::json {
        return {{"user", req.headers["X-User-Id"]}};
    });
});
```

### C++20 Coroutines & Database ORM

```cpp
#include <orbit/orm/Model.hpp>
#include <orbit/database/PostgresClient.hpp>

struct User { int id; std::string name; int age; };
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(User, id, name, age)
ORBIT_REGISTER_MODEL(User, "users")

app.get("/adults", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    auto coro = [writer]() -> concurrency::Task {
        auto db = std::make_shared<database::PostgresClient>(&writer->proactor(), "dbname=myapp");
        co_await connect_async(db);

        // Type-safe Expression Template DSL → compiled to SQL
        auto users = co_await query_User(db)
            .where(orm::Col("age") >= 18)
            .get_async();

        writer->send(http::HttpResponse().status(200).send(nlohmann::json(users).dump()));
    };
    coro();
});
```

### WebSocket EventRouter (Socket.IO-style)

```cpp
#include <orbit/websocket/EventRouter.hpp>

struct PlayerSession { std::string name; int score = 0; };

websocket::EventRouter<PlayerSession> events;

events.on<std::string>("chat", [](auto& ws, const std::string& msg) {
    ws.to("lobby").emit("chat", ws.session().name + ": " + msg);
});

events.on_connect([](auto& ws) {
    ws.join("lobby");
});

events.attach(app, "/ws/game");
```

---

## 📦 Integration

### System-wide Installation (find_package)

Orbit fully supports standard CMake installation, allowing you to install the framework to your system library paths (`/usr/local/lib` and `/usr/local/include`) so that it is automatically picked up by your C++ linker and loader. This is highly recommended for faster compilation times compared to compiling a header-only library.

```bash
git clone https://github.com/varuns2903/orbit-framework.git && cd orbit-framework
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake ..
make -j$(nproc)
sudo make install
```

Once installed, include it in your own project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.15)
project(my_app)

find_package(OrbitFramework REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::server_core)
```

### CMake FetchContent (Alternative)

If you prefer building Orbit directly alongside your project:

```cmake
include(FetchContent)
FetchContent_Declare(
  OrbitFramework
  GIT_REPOSITORY https://github.com/varuns2903/orbit-framework.git
  GIT_TAG        v1.4.0
)
FetchContent_MakeAvailable(OrbitFramework)

target_link_libraries(my_app PRIVATE OrbitFramework::server_core)
```

### vcpkg & Conan

Orbit supports both `vcpkg` (via `vcpkg.json`) and Conan 2.x (via `conanfile.py`). See [Getting Started](docs/getting_started.md) for detailed instructions.

### Orbit CLI

```bash
./tools/cli/orbit new my_project   # Scaffold a new project
cd my_project
orbit build                        # Build with vcpkg
orbit run                          # Start the server
```

---

## 🏗️ Architecture

Orbit is built as a modular stack of composable layers:

```
┌─────────────────────────────────────┐
│           Your Application          │
├─────────────────────────────────────┤
│   Middleware Chain (CORS, Auth...)  │
├─────────────────────────────────────┤
│   Router (Radix Trie + Hash Map)   │
├─────────────────────────────────────┤
│  HTTP/1.1 │ HTTP/2 │ HTTP/3 (QUIC) │
├─────────────────────────────────────┤
│  TLS/SSL  │ WebSockets │ SSE       │
├─────────────────────────────────────┤
│  Proactor Event Engine              │
│  io_uring │ epoll │ kqueue │ IOCP  │
└─────────────────────────────────────┘
```

All features are **modular** — disable what you don't need via CMake flags:

```bash
cmake -B build \
  -DORBIT_ENABLE_HTTP3=OFF \
  -DORBIT_ENABLE_MONGODB=OFF \
  -DORBIT_ENABLE_GRPC=OFF
```

---

## 📖 Documentation

| Guide | Description |
|-------|-------------|
| [🚀 Getting Started](docs/getting_started.md) | Installation, vcpkg, Conan, and FetchContent |
| [🛣️ Routing & Streaming](docs/routing.md) | Routes, parameters, groups, and chunked responses |
| [🛡️ Middleware](docs/middleware.md) | Built-in middleware and custom middleware authoring |
| [💾 Database & Coroutines](docs/database.md) | PostgreSQL, Redis, and C++20 async/await |
| [🔀 Proxy & Load Balancing](docs/proxy.md) | Reverse proxy, connection pooling, and load balancing |
| [🔌 WebSockets](docs/websockets.md) | RFC 6455 WebSockets and EventRouter |
| [⚡ HTTP/3 & QUIC](docs/http3.md) | Enabling and using HTTP/3 |
| [📋 Changelog](CHANGELOG.md) | Release history and breaking changes |

---

## 🤝 Contributing

Contributions are welcome! Please read the existing codebase, ensure CI passes on all platforms, and submit a PR.

## 📄 License

Distributed under the MIT License. See [LICENSE](LICENSE) for details.

---

<div align="center">
  <sub>Built with ❤️ for the C++ community</sub>
</div>
