# Changelog

All notable changes to the Orbit Framework are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v1.5.0] - 2026-09-17

A correctness and integration release. Three bugs fixed here made Orbit
unreliable in ways that only appeared once you tried to *use* it from another
project, so **upgrading from v1.4.0 or earlier is strongly recommended**.

### Fixed

- **Consuming applications corrupted their own stack.** `App.hpp` declares two
  members under `#ifdef ORBIT_ENABLE_HTTP3`, but the macro was set with CMake's
  directory-scoped `add_compile_definitions()`, so it never reached anything
  linking Orbit. Consumers therefore compiled `App` 16 bytes smaller than the
  constructor in `libserver_core.a` was built to fill, and `App`'s constructor
  wrote past the end of the caller's object — reported as
  `*** stack smashing detected ***` on shutdown. This affected every
  integration path: FetchContent, `find_package`, vcpkg and Conan. Feature
  macros are now `PUBLIC` on the target and propagate correctly.
- **HTTP/2 response headers were a use-after-free.** Each `nghttp2_nv` name
  borrowed a pointer into a `std::string` scoped to the loop that built it, so
  every name dangled by the time nghttp2 read the list. It rarely crashed —
  the freed block is immediately recycled — so header names went out as empty
  or garbage instead.
- **HTTP/1.0 clients hung until they timed out.** RFC 9112 section 9.3 makes
  HTTP/1.0 close by default and persist only on an explicit `keep-alive`;
  Orbit applied the HTTP/1.1 rule to both and held the socket open. Affected
  health checks, older proxies and load balancers, and benchmarking tools.
  The `Connection` header is now parsed as the comma-separated list of
  case-insensitive tokens it is, so `Connection: Close` and
  `Connection: TE, close` are also honoured.
- Public headers included `<nlohmann/json.hpp>`, which resolved to the copy
  bundled inside inja (3.10.5) rather than the 3.11.3 copy Orbit vendors.
  Both use the same include guard, so different translation units could see
  different definitions of `nlohmann::json`. All Orbit headers now use
  `<orbit/http/json.hpp>`.
- `find_package(OrbitFramework)` failed on any machine where MariaDB, MongoDB
  or hiredis came from the system rather than vcpkg, and demanded
  dependencies for subsystems the build had disabled. The installed config now
  records which subsystems were enabled and mirrors the same lookup fallbacks
  the build uses.
- The installed CMake package exported `pantor::inja`, a FetchContent target
  that is never installed and which no consumer could resolve.
- `find_package` and FetchContent exported different target names. Both now
  provide `OrbitFramework::core`, with `OrbitFramework::server_core` kept as a
  compatibility alias.
- Examples were built unconditionally, so configuring with a subsystem
  disabled failed at link. Each example is now guarded by the subsystems it
  uses.
- CI could not resolve dependencies once upstream vcpkg moved past the pinned
  `builtin-baseline`, because the workflows shallow-fetched `master` rather
  than the pinned commit.
- `conanfile.py` reported version `0.1.0` and exported no sources. It now
  reads the version from `CMakeLists.txt` and exports the tree it needs.

### Added

- `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, issue forms and a
  pull request template.
- `THIRD_PARTY_NOTICES.md` covering bundled, fetched and linked dependencies,
  including guidance for projects that already use nlohmann/json.
- `docs/coverage.md` and a Code Coverage workflow publishing a measured figure
  on every push — currently **27.1% lines** across 109 tests.
- An API Stability section stating that 1.x is pre-stable and that minor
  releases may break source compatibility.
- A comprehensive Installation section documenting every supported route:
  installer, CLI, FetchContent, `find_package`, vcpkg, Conan, Docker, building
  from source, and CPack packaging.
- 45 new tests covering WebSocket frame decoding, HTTP/2 header encoding,
  HTTP/2 method parsing, and `Connection` header option parsing. The suite
  grows from 64 to 109.

### Changed

- Test sources are collected with `file(GLOB CONFIGURE_DEPENDS)`. Two test
  files had never been listed in `add_executable` and were silently never
  compiled.
- Roadmap entries claiming vcpkg/Conan publication, penetration testing and
  85-90% coverage are unticked and describe what actually works today.
- Performance claims are backed by measurement — median **61,419 req/s**
  plaintext on the documented hardware — with methodology and limits stated in
  `docs/benchmarks.md`.
- GitHub Actions updated off the deprecated Node 20 runtime.

## [v1.4.0] - 2026-09-02

### Added
- **Magic Return Values (FastAPI-style)**: Route handlers can now directly return `std::string`, `nlohmann::json`, or custom structs — Orbit automatically serializes and sends the HTTP response.
- **Socket.IO-style WebSocket EventRouter**: Strongly-typed event routing with auto-JSON mapping, rooms, and per-connection session state.
- **Unified DBAL & ORM**: Expression Template-based Query DSL (`Col("age") >= 18`), unified `ResultSet`, automatic JSON serialization for database queries, and MongoDB ORM integration.
- **GraphQL & gRPC adapters**: GraphQL HTTP middleware adapter and optional gRPC server wrapper.
- **Orbit CLI**: `orbit new`, `orbit build`, `orbit run` for scaffolding and managing projects.
- **Modular CMake options**: `ORBIT_ENABLE_HTTP3`, `ORBIT_ENABLE_REDIS`, `ORBIT_ENABLE_GRPC`, etc.
- **Package manager support**: vcpkg (`vcpkg.json`), Conan 2.x (`conanfile.py`), and CMake `FetchContent`.
- **CPack support**: Generate installable release packages.
- **`BUILD_SHARED_LIBS` support**: Build Orbit as either a static or shared library.
- **Cross-platform CI**: GitHub Actions workflow testing Ubuntu, macOS, and Windows with Valgrind leak detection.
- **Native C++ test suite**: Migrated from Python to GoogleTest with `gcovr` code coverage integration.
- Unit tests for `ServerConfig`, `ConnectionPool`, `EventRouter`, `TlsContext`, `MultipartStreamParser`, ORM expressions, and E2E HTTP integration tests.

### Fixed
- **Critical `ConnectionPool` deadlock**: Callbacks were invoked while holding the mutex, causing deadlocks when callbacks triggered pool operations.
- **`TlsContext` memory leak**: `SSL_CTX` was not freed when the constructor threw on invalid certificates (caught by Valgrind in CI).
- **`HandlerWrapper` deduction failure**: Coroutine `Task` handlers that manage their own `ResponseWriter` now compile correctly.
- Various CMake linkage and export issues for downstream consumers.

## [v1.3.0] - 2026-08-13

### Added
- **Doxygen API documentation** with GitHub Pages deployment.
- **OpenAPI/Swagger auto-generation**: `app.enable_openapi()` generates a `/swagger.json` endpoint from registered routes.
- **CSRF protection middleware**.
- **OAuth2 client middleware** using libcurl.
- **Native Cookie API**.
- **Database Connection Pooling**.
- **MongoDB async client** (`MongoClient`).
- **MySQL/MariaDB async client** (`MysqlClient`).

### Fixed
- Multiple cross-platform CI build errors (mongoc, mariadb, hiredis target names).
- QUIC initialization crash and HTTP version selection.
- Segmentation fault during load testing (destruction order, data races in `SessionManager` and pipelined requests).
- Default engine changed to `epoll` on Linux to avoid `io_uring` concurrency bugs on older kernels.

## [v1.2.1] - 2026-08-11

### Fixed
- CI build errors on Windows (POSIX APIs, MSVC compiler flags, socket headers).
- `ngtcp2` crypto feature configuration for vcpkg.

## [v1.2.0] - 2026-08-11

### Added
- **Windows support**: `IocpProactor` (I/O Completion Ports) and WinSock2 compatibility layer.
- **Windows CI**: Added `windows-latest` to the GitHub Actions matrix.
- Valgrind memory profiling and `wrk` load testing in CI.
- Dockerfile and docker-compose for containerization.

## [v1.1.1] - 2026-08-09

### Fixed
- CI pipeline fixes for macOS Homebrew package names and Ubuntu `ngtcp2`/`nghttp3` builds.

## [v1.1.0] - 2026-08-09

### Added
- **HTTP/3 & QUIC** support via `ngtcp2` and `nghttp3`.
- **HTTP/2** support via `nghttp2`.
- **`io_uring` Proactor** for high-performance async I/O on modern Linux kernels.
- **WebSocket** support (RFC 6455) with framing, masking, and fragmentation.
- **Redis client** with async operations.
- **PostgreSQL C++20 coroutine client** (`co_await connect_async`, `co_await query_async`).
- **Reverse proxy & load balancer** middleware with connection pooling and TLS session reuse.
- **JWT authentication middleware**.
- **Rate limiting middleware** (in-memory token bucket).
- **Gzip/Deflate compression middleware**.
- **Session management middleware**.
- **JSON schema validation middleware**.
- **`KqueueProactor`** for macOS/BSD cross-platform support.
- GitHub Actions CI for Linux and macOS.
- SSL connection pooling, global error handlers, and C++20 coroutine integration.

## [v1.0.0] - 2026-08-09

### Added
- Initial release of the Orbit Framework.
- Asynchronous HTTP/1.1 server with `epoll`-based event loop.
- Express-style routing with dynamic parameters (`:id`), middleware pipeline, and route grouping.
- Static file server middleware with MIME type detection.
- CORS middleware.
- Thread pool for offloading CPU-bound tasks.
- Timer management and graceful shutdown (SIGINT/SIGTERM).
- Configurable logging, port, worker threads, and max body size.
- CMake build system with install/export rules.
- `nlohmann/json` integration for JSON request/response handling.

[v1.4.0]: https://github.com/varuns2903/orbit-framework/compare/v1.3.0...v1.4.0
[v1.3.0]: https://github.com/varuns2903/orbit-framework/compare/v1.2.1...v1.3.0
[v1.2.1]: https://github.com/varuns2903/orbit-framework/compare/v1.2.0...v1.2.1
[v1.2.0]: https://github.com/varuns2903/orbit-framework/compare/v1.1.1...v1.2.0
[v1.1.1]: https://github.com/varuns2903/orbit-framework/compare/v1.1.0...v1.1.1
[v1.1.0]: https://github.com/varuns2903/orbit-framework/compare/v1.0.0...v1.1.0
[v1.0.0]: https://github.com/varuns2903/orbit-framework/releases/tag/v1.0.0
