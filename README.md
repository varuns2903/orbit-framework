<div align="center">
  
  <h1>🚀 Orbit Framework</h1>
  
  <p><b>A fast, asynchronous C++20 web framework with HTTP/3, WebSockets, and a built-in ORM</b></p>
  
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
| **Fast** | ~61k req/s on a trivial keep-alive workload ([measured](docs/benchmarks.md)); asynchronous Proactor pattern over `io_uring`, `epoll`, `kqueue`, and Windows IOCP |
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

### 1. Install

```bash
curl -sL https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.sh | bash
```

Windows, and every other way to get Orbit — FetchContent, vcpkg, Conan, Docker,
`find_package` — are covered under [Installation](#-installation). If you would
rather not install anything system-wide, [FetchContent](#cmake-fetchcontent)
drops Orbit straight into an existing CMake project.

### 2. Create a Project

```bash
orbit new myapp
cd myapp
```

This scaffolds `main.cpp`, a `CMakeLists.txt` wired to Orbit, and a `vcpkg.json`
listing the dependencies.

### 3. Write Your Server

Edit `main.cpp`:

```cpp
#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/json.hpp>

int main() {
    config::ServerConfig config;
    config.port = 8080;

    server::App app(config);

    // Return a string — Orbit handles the HTTP response automatically
    app.get("/", []() -> std::string {
        return "Hello from Orbit! 🚀";
    });

    // Return JSON
    app.get("/api/status", []() -> nlohmann::json {
        return {{"status", "ok"}, {"version", "1.5.1"}};
    });

    // Dynamic route parameters
    app.get("/users/:id", [](const http::HttpRequest& req) -> nlohmann::json {
        return {{"user_id", req.params.at("id")}};
    });

    app.listen();
}
```

### 4. Build & Run

```bash
orbit build --release
orbit run
```

```bash
$ curl http://localhost:8080/
Hello from Orbit! 🚀

$ curl http://localhost:8080/api/status
{"status":"ok","version":"1.5.1"}

$ curl http://localhost:8080/users/42
{"user_id":"42"}
```

---

## 📦 Installation

Orbit can be consumed in several ways. Pick by what you are doing:

| You want to… | Use | Needs a system install? |
|---|---|---|
| Try Orbit quickly on Linux/macOS/Windows | [One-command installer](#one-command-installer) | Yes |
| Start a new app from a template | [Orbit CLI](#orbit-cli) | Yes (installer provides it) |
| Add Orbit to an existing CMake project | [CMake FetchContent](#cmake-fetchcontent) | **No** |
| Link a system-wide build | [find_package](#system-wide-install-find_package) | Yes |
| Manage deps with vcpkg | [vcpkg](#vcpkg) | No |
| Manage deps with Conan | [Conan 2.x](#conan-2x) | No |
| Ship a container | [Docker](#docker) | No |
| Hack on Orbit itself | [Build from source](#build-from-source) | No |
| Produce `.deb` / `.rpm` / `.tar.gz` | [CPack packages](#building-distributable-packages) | No |

> **Version note.** Use `v1.5.1` or later. `v1.4.0` and earlier contain a CMake
> defect that corrupted the stack of *every* consuming application, along with
> an HTTP/2 use-after-free and an HTTP/1.0 connection hang. See
> [CHANGELOG.md](CHANGELOG.md) and [API Stability](#-api-stability).

### Prerequisites

Common to every method that builds from source:

- **C++20 compiler** — GCC 11+, Clang 14+, or MSVC 19.30+
- **CMake** 3.20+
- **Linux kernel 5.6+** for the `io_uring` backend (falls back to `epoll`)

Orbit links OpenSSL, zlib, libcurl, nghttp2, and — depending on enabled
features — ngtcp2, nghttp3, libpq, MariaDB Connector/C, mongo-c-driver,
hiredis, and liburing. Let vcpkg or Conan supply them rather than installing by
hand. Full list and licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

### One-Command Installer

Builds Orbit in Release mode, installs the library to `/usr/local`, and puts the
`orbit` CLI on your `PATH`.

**Linux / macOS**
```bash
curl -sL https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.sh | bash
```

**Windows (PowerShell as Administrator)**
```powershell
iwr -useb https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.ps1 | iex
```

The script clones Orbit, bootstraps its own vcpkg, compiles every dependency,
and installs. Expect 20–40 minutes on first run; a binary cache under
`~/.cache/vcpkg-binary-cache` makes repeat runs far quicker.

> Piping a script into a shell runs arbitrary code as you, with `sudo` for the
> install step. Read [install.sh](install.sh) first if that matters to you, or
> use [FetchContent](#cmake-fetchcontent), which needs no system install at all.

---

### Orbit CLI

Available once the installer has run.

```bash
orbit new myapp          # scaffold main.cpp, CMakeLists.txt, vcpkg.json
orbit new myapp --fetch  # scaffold using FetchContent instead of find_package
cd myapp
orbit build              # Debug
orbit build --release    # Release with LTO
orbit run
```

`orbit build` picks up `VCPKG_ROOT` if set, otherwise a `vcpkg/` directory beside
your project, and warns if it finds neither.

---

### CMake FetchContent

The lightest option: no system install, and the version is pinned in your own
build files. Orbit skips its examples, tests, and install rules when built as a
subproject, so you get just the library.

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  OrbitFramework
  GIT_REPOSITORY https://github.com/varuns2903/orbit-framework.git
  GIT_TAG        v1.5.1        # pin a release; avoid v1.4.0 and earlier
)
FetchContent_MakeAvailable(OrbitFramework)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```

Configure with a vcpkg toolchain so Orbit's own dependencies resolve:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
```

Turn off what you do not need to cut build time substantially:

```bash
cmake -B build -DORBIT_ENABLE_MONGODB=OFF -DORBIT_ENABLE_MARIADB=OFF
```

---

### System-Wide Install (find_package)

Build and install once, then link from any project.

```bash
git clone https://github.com/varuns2903/orbit-framework.git
cd orbit-framework
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh          # bootstrap-vcpkg.bat on Windows

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DORBIT_BUILD_TESTS=OFF \
  -DORBIT_BUILD_EXAMPLES=OFF
cmake --build build --parallel
sudo cmake --install build          # honours CMAKE_INSTALL_PREFIX
```

Then, in your own project:

```cmake
find_package(OrbitFramework REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```

If you installed to a custom prefix, point CMake at it:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/opt/orbit
```

The installed package records which subsystems it was built with and asks only
for those dependencies, so an Orbit built with `-DORBIT_ENABLE_MONGODB=OFF`
will not demand mongo-c-driver from your project.

> `OrbitFramework::server_core` also resolves, as older examples used that name.
> `OrbitFramework::core` is canonical.

---

### vcpkg

**Manifest mode** — Orbit's own `vcpkg.json` lists its dependencies, and this is
how CI builds:

```bash
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
```

The dependency set is pinned with `builtin-baseline`, so everyone resolves the
same versions.

**As a vcpkg port** — a port is drafted under `packaging/vcpkg-port/` but has
**not** been submitted upstream, so `vcpkg install orbit-framework` does not
resolve yet. To try the draft as an overlay:

```bash
vcpkg install orbit-framework --overlay-ports=packaging/vcpkg-port
```

This path is unvalidated — see [#20](https://github.com/varuns2903/orbit-framework/issues/20).

---

### Conan 2.x

```bash
conan install . --output-folder=build --build=missing
cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Or export Orbit into your local Conan cache and depend on it by name:

```bash
conan create .
```

The recipe reads its version from `CMakeLists.txt`, so it always matches the
project. Orbit is **not** on ConanCenter — see
[#6 on the roadmap](docs/ROADMAP.md).

> `ngtcp2` and `nghttp3` have no ConanCenter recipes, so the Conan path builds
> without HTTP/3 unless you supply them yourself. Use vcpkg if you need QUIC.

---

### Docker

A multi-stage `Dockerfile` builds quictls, nghttp3, and ngtcp2 from source for
full HTTP/3 support, then ships a slim runtime image.

```bash
docker build -t orbit-app .
docker run -p 8080:8080 -p 8443:8443 -p 8443:8443/udp orbit-app
```

`docker-compose.yml` brings up Orbit alongside PostgreSQL and Redis with
health-gated startup:

```bash
docker compose up --build
```

Use it as a base for your own service by copying your sources in and building
against the installed framework. The build is long — the QUIC stack is compiled
from source — so lean on Docker layer caching.

---

### Build From Source

For working on Orbit itself. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full
workflow.

```bash
git clone https://github.com/varuns2903/orbit-framework.git
cd orbit-framework
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

cd build && ctest --output-on-failure
./benchmark_server
```

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer by default.

**Without vcpkg**, using system packages — you are responsible for satisfying
every dependency, and distribution packages are often too old for HTTP/3:

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libssl-dev zlib1g-dev \
     libcurl4-openssl-dev libnghttp2-dev liburing-dev libpq-dev \
     libmariadb-dev libmongoc-dev libhiredis-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release -DORBIT_ENABLE_HTTP3=OFF
cmake --build build --parallel
```

### Build Options

| Option | Default | Effect |
|--------|---------|--------|
| `ORBIT_ENABLE_HTTP3` | `ON` | HTTP/3 and QUIC (needs ngtcp2 + nghttp3) |
| `ORBIT_ENABLE_REDIS` | `ON` | Redis client |
| `ORBIT_ENABLE_POSTGRES` | `ON` | PostgreSQL client |
| `ORBIT_ENABLE_MARIADB` | `ON` | MySQL/MariaDB client |
| `ORBIT_ENABLE_MONGODB` | `ON` | MongoDB client |
| `ORBIT_ENABLE_GRPC` | `OFF` | gRPC server wrapper |
| `ORBIT_BUILD_TESTS` | `ON` | Test suite (downloads GoogleTest) |
| `ORBIT_BUILD_EXAMPLES` | `ON` | Example servers |
| `ENABLE_SANITIZERS` | `ON` | ASan + UBSan in Debug builds |
| `ORBIT_ENABLE_COVERAGE` | `OFF` | gcov instrumentation |
| `BUILD_SHARED_LIBS` | `OFF` | Shared instead of static library |
| `ENABLE_FUZZING` | `OFF` | libFuzzer targets (requires Clang) |

These flags change the layout of public headers, and they propagate to your
project automatically through `OrbitFramework::core` — do not set them by hand
in a consuming project.

### Building Distributable Packages

CPack is configured, so you can produce native packages from a build tree:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DORBIT_BUILD_TESTS=OFF
cmake --build build --parallel
cd build && cpack
```

Generates `.tar.gz` and `.zip` everywhere, plus `.deb` and `.rpm` on Linux,
a `.dmg` on macOS, and an NSIS installer on Windows. No prebuilt packages are
attached to GitHub Releases yet, so build your own for now.

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
| [📚 API Reference](https://varuns2903.github.io/orbit-framework/) | Auto-generated Doxygen API Documentation |
| [🛣️ Routing & Streaming](docs/routing.md) | Routes, parameters, groups, and chunked responses |
| [🛡️ Middleware](docs/middleware.md) | Built-in middleware and custom middleware authoring |
| [💾 Database & Coroutines](docs/database.md) | PostgreSQL, Redis, and C++20 async/await |
| [🔀 Proxy & Load Balancing](docs/proxy.md) | Reverse proxy, connection pooling, and load balancing |
| [🔌 WebSockets](docs/websockets.md) | RFC 6455 WebSockets and EventRouter |
| [⚡ HTTP/3 & QUIC](docs/http3.md) | Enabling and using HTTP/3 |
| [📋 Changelog](CHANGELOG.md) | Release history and breaking changes |
| [📊 Test Coverage](docs/coverage.md) | Measured coverage, per-file gaps, and how to reproduce |
| [⚡ Benchmarks](docs/benchmarks.md) | Throughput figures, methodology, and their limits |

---

## 🔒 API Stability

Orbit is versioned with [Semantic Versioning](https://semver.org/), but it has
not yet reached a frozen public API. **Treat 1.x as pre-stable.**

- **The API may change in minor releases.** Roadmap item 31, *maintain API/ABI
  compatibility*, is not yet met. Every breaking change is documented in
  [CHANGELOG.md](CHANGELOG.md), but a minor bump is not a guarantee of a
  drop-in upgrade.
- **There is no ABI stability guarantee.** Rebuild your application against a
  new Orbit release rather than swapping the shared library underneath it.
- **Pin your version.** Use an exact tag with `FetchContent` or your package
  manager, and upgrade deliberately after reading the changelog.
- **Subsystem maturity varies.** HTTP/1.1, routing, middleware, and WebSockets
  are the best exercised. HTTP/2, HTTP/3, the gRPC wrapper, and parts of the
  ORM have thinner test coverage — see [docs/ROADMAP.md](docs/ROADMAP.md) for
  the honest state of each area and
  [docs/loopholes_and_drawbacks.md](docs/loopholes_and_drawbacks.md) for known
  architectural caveats.

If you are evaluating Orbit for production, read
[docs/loopholes_and_drawbacks.md](docs/loopholes_and_drawbacks.md) first.

---

## 🤝 Contributing

Contributions are welcome — and you don't need to write C++ to help. Test
coverage, documentation, examples, and platform testing are all high-impact
right now.

- Read [CONTRIBUTING.md](CONTRIBUTING.md) for build setup, coding standards, and the PR process
- Browse [good first issues](https://github.com/varuns2903/orbit-framework/labels/good%20first%20issue) for a scoped starting point
- See [docs/ROADMAP.md](docs/ROADMAP.md) for what's planned and what's unclaimed
- All participation is governed by our [Code of Conduct](CODE_OF_CONDUCT.md)

Found a security issue? **Do not open a public issue** — follow
[SECURITY.md](SECURITY.md).

## 📄 License

Distributed under the MIT License. See [LICENSE](LICENSE) for details.

Orbit bundles and links third-party components under their own licenses —
including a vendored copy of nlohmann/json. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list and for
guidance if your project already uses nlohmann/json.

---

<div align="center">
  <sub>Built with ❤️ for the C++ community</sub>
</div>
