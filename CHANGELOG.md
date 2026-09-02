# Changelog

All notable changes to the Orbit Framework are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
