# Getting Started with Orbit

Orbit is a high-performance C++20 web framework (HTTP/1.1, HTTP/2, and experimental HTTP/3) built on top of asynchronous kernel event loops (`io_uring` and `epoll`). It aims to provide Express.js-like ergonomics with raw C++ performance.

## Prerequisites

- **C++20 Compiler**: GCC 11+ or Clang 14+
- **Linux Kernel 5.6+**: Required for `io_uring` support (will fallback to `epoll` on older kernels).
- **CMake 3.15+**
- **Libraries**: OpenSSL, liburing, libpq

## Installation & Build

We highly recommend using `vcpkg` to automatically install all required dependencies (like OpenSSL, PostgreSQL, MongoDB, ngtcp2, etc.) so you don't have to compile them from source.

```bash
git clone https://github.com/varuns2903/orbit-framework.git orbit
cd orbit

# Clone vcpkg if you don't have it installed
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# Build Orbit Framework with vcpkg toolchain
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel 4   # about 2 GB of RAM per job
```

### Build with Conan (Alternative to vcpkg)

If you prefer Conan 2.x for dependency management:

```bash
# Install dependencies using the provided conanfile.py
conan install . --output-folder=build --build=missing

# Build the framework
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel 4   # about 2 GB of RAM per job
```

### Manual Build (Without vcpkg)

If you prefer using system packages, install the prerequisites manually:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel 4   # about 2 GB of RAM per job
```

### Integrating via CMake FetchContent (Recommended)

If you have your own CMake project and want to include Orbit seamlessly without manually building it first, you can use CMake's `FetchContent`. Since Orbit manages its internal examples and tests safely, fetching it will only build the core library.

Add this to your `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
  OrbitFramework
  GIT_REPOSITORY https://github.com/varuns2903/orbit-framework.git
  GIT_TAG        main # Pin a release tag once one carries the current fixes
)
FetchContent_MakeAvailable(OrbitFramework)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```
*(Make sure to still pass `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake` when building your own project so the dependencies resolve).*

## Your First Orbit Server

Create a `main.cpp` file:

```cpp
#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/json.hpp>
#include <iostream>

int main() {
    orbit::config::ServerConfig config;
    config.port = 8080;

    orbit::server::App app(config);

    // Return a string — Orbit builds the HTTP response for you
    app.get("/", []() -> std::string {
        return "Hello from Orbit!";
    });

    // Return JSON
    app.get("/api/status", []() -> nlohmann::json {
        return {{"status", "ok"}};
    });

    std::cout << "Server starting on port 8080\n";
    app.listen();

    return 0;
}
```

Build it against Orbit (see the [Installation section](../README.md#-installation)
for every available method) and test:

```bash
curl http://localhost:8080
curl http://localhost:8080/api/status
```

### One Include, Shorter Names

`<orbit/orbit.hpp>` pulls in the server, routing, the built-in middleware,
WebSockets and the test client in one include (everything except the
database clients and ORM, which stay separate so apps that don't use them
don't pay for their dependencies: `<orbit/database/*.hpp>`,
`<orbit/orm/*.hpp>`). It also defines short aliases inside `namespace
orbit` — `App`, `Request`, `Response`, `Status`, `Method`, `Writer`
(`std::shared_ptr<ResponseWriter>`), `RouteHandler`, `Middleware`, `json` —
for the namespaced types every handler signature repeats. The example above,
written with them:

```cpp
#include <orbit/orbit.hpp>

int main() {
    orbit::App app(orbit::config::ServerConfig{.port = 8080});

    app.get("/tasks/:id", [](orbit::Request& req, orbit::Writer w) {
        w->send(orbit::Response::error(orbit::Status::NotFound, "no task " + req.params["id"]));
    });

    app.get("/api/status", [](orbit::Request&, orbit::Writer w) {
        w->send(orbit::Response().json(orbit::json{{"status", "ok"}}, orbit::Status::OK));
    });

    app.listen();
}
```

An alias is the exact same type as its full name (`orbit::App` *is*
`orbit::server::App`), so the two spellings mix freely in one codebase;
nothing stops using `orbit::server::App` instead, or together with the
short names, as earlier examples in this guide do.

`Response::error(status, message)` and `Response().json(body, status)` are
the one-line shapes most handlers end up writing by hand: a JSON `{"error":
...}` body with its status, and a JSON body plus status set together.

## Configuration Files

Settings can live in a JSON file, be overridden by `ORBIT_*` environment
variables (containers, CI), and then by command-line flags:

```json
{
  // Comments are allowed.
  "server": {
    "port": 8080,
    "host": "0.0.0.0",
    "worker_threads": "auto",
    "header_timeout": 5,
    "engine": "auto"
  },
  "database": { "url": "postgres://localhost/app", "pool_size": 8 }
}
```

```cpp
#include <orbit/config/ConfigFile.hpp>

int main(int argc, char* argv[]) {
    auto cfg = orbit::config::Config::load("orbit.json", argc, argv);
    orbit::server::App app(cfg.server);
    std::string db_url = cfg.section("database").value("url", "");
    // ...
}
```

- `server` keys are the `ServerConfig` field names; durations are in
  seconds; `worker_threads` and `event_loops` take `"auto"`; `engine` is
  `"epoll"`, `"iouring"` or `"auto"`; `http_version` is `"1.1"`, `"2"` or `"3"`.
- `ORBIT_PORT=9000`, `ORBIT_LOG_LEVEL=DEBUG` and the like set server keys;
  `ORBIT_DATABASE__URL=...` (a double underscore between levels) sets any
  path, so application sections can be overridden too. Values that parse as
  JSON (numbers, `true`) are used as such.
- Validation is strict: an unknown server key or a value of the wrong type
  stops start-up with `orbit::config::ConfigError`, naming the file and key
  (`orbit.json: server.port: expected a whole number from 0 to 65535, not "80"`).
## Start-Up, Shutdown and Timers

Code that should run once the server is up, or as it stops, goes in hooks;
periodic work goes in timers. All of it runs on the worker pool.

```cpp
app.on_start([&](orbit::server::App& app) {
    auto migrated = orbit::orm::migrate_sync(conninfo, "migrations");
    if (!migrated.ok()) app.stop();            // abort start-up
});
app.on_stop([&](orbit::server::App&) {
    queue.flush();                             // before connections drain
});

auto heartbeat = app.run_every(std::chrono::seconds(15), [&] { hub.ping(); });
app.run_after(std::chrono::minutes(1), [] { warm_caches(); });
// heartbeat.cancel() stops it.
```

- `on_start` hooks run once the listeners are bound, in the order added; a
  hook that throws is logged and the rest still run.
- `on_stop` hooks run once: at the first `shutdown()` (or SIGTERM/SIGINT)
  before the drain, at `stop()`, or when `listen()` returns. They run on the
  thread that stops the server, so keep them short.
- Timers count from `listen()` (or from now, if the server is running) and
  stop with the server. Runs of one timer never overlap: a tick that comes
  while the previous run is still going is skipped. Exceptions are logged.

## Command-Line Flags

`ServerConfig::parse(argc, argv)` reads the server's flags (`--port`, `--host`
/ `--bind`, `--threads`, `--engine`, ...; run with `--help` for the list):

```cpp
int main(int argc, char* argv[]) {
    orbit::server::App app(orbit::config::ServerConfig::parse(argc, argv));
    // ...
}
```

A bad value, or a flag missing its value, stops the program with a message.
An unknown flag is reported with the closest known one (`--hots`: "did you
mean --host?") and otherwise ignored, so an application can mix in flags of
its own. If the whole command line is Orbit's, pass
`ServerConfig::ParseMode::Strict` and unknown flags stop the program too.

## Upgrading

Moving to a newer Orbit release? The [Migration Guide](migration.md) lists the
changes each release needs in your code and build files.
