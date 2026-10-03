# Changelog

All notable changes to the Orbit Framework are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v1.6.0] - 2026-10-03

Security and robustness release. **Everyone on v1.5.1 or earlier should
upgrade.** Security advisories for the issues below will be published on the
repository's Security tab once this release is available.

### Upgrade notes

Most changes are fixes, but some intentionally tighten behaviour:

- **HTTP status codes.** A wrong method gets `405` with `Allow` (it used to get
  `404`). `HEAD` is answered from `GET` routes.
- **Request paths.** Handlers receive percent-decoded `uri`, `query` and `params`.
  Paths containing an encoded `/`, `\` or NUL are rejected with `400`.
- **Error responses.** `500` bodies no longer include exception messages.
  `on_error` also receives exceptions thrown by handlers that return values.
- **CORS.** With an allow-list, unlisted origins receive no CORS headers.
- **JWT.** Only HS256 is accepted, and a future `nbf` is rejected.
  `jwt_auth("")` throws.
- **WebSocket routes** now run the app's middleware and validate the handshake.
- **OAuth2.** Logins must start through `login_handler()`, which sets the state cookie
  that callbacks now require.
- **Reverse proxy.** HTTPS upstreams with untrusted certificates are rejected. Set
  `verify_tls = false`, or use `ca_file` for a private CA.
- **ORM.** Values are bound as parameters instead of being spliced into SQL. Code that read
  `orm::Expr::sql` expecting inlined values, or passed arbitrary operator strings
  or non-identifier column names, must be updated.
- **Database.** `PostgresClient` failures return `ResultSet::failure(message)`; check
  `ok()` / `error()`. `to_json()` keeps `""` for empty strings and uses `null`
  only for SQL NULL.
- **Distributed rate limiter.** Answers `503` (fail closed) when Redis is
  unreachable. Pass `allow_when_unavailable = true` to let requests through instead.
- **Static files.**
  - Dotfiles (`.env`, `.git/`) are no longer served.
  - Text types carry `; charset=utf-8`.
  - The ETag format changed, so cached copies revalidate once.
- **Timeouts.** The single 10-second timer is replaced by `header_timeout`,
  `keep_alive_timeout`, `idle_timeout` and `websocket_idle_timeout`.
- **Listen backlog.** The default is now `SOMAXCONN` (it was 10).

### Security

- HTTP/1.1 request framing: conflicting or duplicate `Content-Length`, and
  `Transfer-Encoding` combined with `Content-Length`, are rejected. This prevents
  request smuggling, and a duplicate `Content-Length` no longer terminates the
  process.
- WebSocket:
  - Frame lengths are validated and message size is bounded.
  - A connection that drops without a close frame no longer causes a use-after-free.
  - The upgrade no longer bypasses middleware and origin checks.
- ORM query builder: values are bound as parameters instead of spliced into SQL
  (SQL injection).
- CORS: the origin allow-list is honoured and `Vary: Origin` is sent.
- CSRF middleware no longer leaves dangling header references.
- Session IDs, CSRF tokens and QUIC connection IDs, stateless reset tokens and
  path challenges are drawn from a CSPRNG.
- JWT verification: the algorithm is pinned, signatures are compared in
  constant time, and `exp`/`nbf` are checked.
- OAuth2: adds `state` and PKCE, encodes parameters, and puts timeouts on
  provider calls.
- Reverse proxy:
  - Verifies upstream TLS.
  - Forwards headers safely.
  - Owns the request it forwards; it used to keep a dangling reference.
- TLS: OpenSSL state is serialised per connection.
- HTTP/2: file responses own their descriptors; the response could read a
  descriptor that had already been closed.
- Response header and cookie values are validated for CR/LF.
- Exception messages are no longer disclosed to clients.
- The signal handler is async-signal-safe.
- The Redis client no longer deadlocks after losing its connection.
- Connections that are idle, slow, or trickle their headers are bounded by
  per-phase timeouts. Long handlers, WebSockets and SSE are no longer cut off.

### Fixed

- **HTTP/1.1 pipelining.**
  - A pipelined response could be dropped, or the socket closed before the final
    response was sent.
  - A connection could linger after its last response.
  - Pipelined file responses corrupted each other's offset.
- A handler that sent a fixed-length response while a chunked request body was
  still arriving broke the body decoding.
- `EAGAIN`/`EINTR` from a read or write closed the connection. They now retry.
- TLS `close_notify` and fatal TLS errors were ignored, so the connection stayed
  open until the keep-alive timeout.
- **Static files.**
  - The path-containment check rejected every file on Windows. It also accepted
    sibling directories that shared a prefix, and symlinks that point outside the
    directory.
  - Many common MIME types were missing.
- **In-memory rate limiter.**
  - Memory grew with every client address it saw; it is now bounded.
  - Refill is continuous.
- **Redis rate limiter.** The counter and its expiry are now set atomically, so a
  client can no longer be locked out permanently.
- Response interceptors and default headers leaked into later responses on a
  keep-alive connection.
- Nested router groups now register their routes.
- HTTP/2 now matches HTTP/1.1 for query strings, cookies, interceptors and
  streaming.
- Database query errors are reported, NULL is preserved, and migrations run in a
  transaction.
- The thread pool's stop flag is set under its mutex.
- Library diagnostics go through the Logger rather than `std::cout`, and respect
  `log_level`.
- Accept:
  - One readiness event now accepts every queued connection.
  - Running out of file descriptors no longer sleeps on the event-loop thread.
- **Installers.**
  - `install.sh` and `install.ps1` install the latest release rather than `main`.
  - vcpkg is pinned to the manifest baseline.
  - Build parallelism is bounded by available memory.
  - Both scripts stop on the first error.

### Added

- `ServerConfig::host` (`--bind`):
  - Accepts `127.0.0.1`, `::1`, host names, and `::` for a dual-stack listener.
  - IPv6 client addresses are reported.
- `ServerConfig::backlog` (`--backlog`) and `ServerConfig::max_connections`
  (`--max-connections`). When the connection limit is reached, new connections
  wait in the backlog until a slot frees.
- `middleware::StaticFilesOptions`:
  - `index`, `serve_dotfiles` and `max_age`.
  - Responses carry `Last-Modified`, `Cache-Control` and `Accept-Ranges`.
  - `HEAD` is supported.
  - Single byte ranges return `206`/`416`, and `If-Range` is honoured.
  - `If-None-Match` lists and `If-Modified-Since` are supported.
  - `mime_type_for_extension()` is exposed.
- `HttpResponse::file_offset` / `set_file_range()` for partial file responses
  over HTTP/1.1 and HTTP/2.
- `middleware::RateLimitOptions`: a custom key function and
  `max_tracked_clients`. Both rate limiters send `Retry-After`.
- `RedisClient::incr_with_expiry()`.
- `WebSocketConnection::set_max_message_size()`, `ws(...)` overloads with
  middleware, and `require_origin`.
- `JwtOptions`, OAuth2 PKCE/state configuration, and `ProxyOptions`
  (`use_tls`, `verify_tls`, `ca_file`).
- `ResultSet::ok()` / `error()`, and parameterised `query`/`query_async`.
- `HttpRequest::set_header()` / `target`.
- `EventLoop::set_tick_hook()`.
- `Listener::port()`.
- Installer variables `ORBIT_VERSION`, `ORBIT_PREFIX` and `ORBIT_JOBS`.

### Removed

- The unused hiredis dependency.

## [v1.5.1] - 2026-09-17

Packaging-only release. No library code changed — the only reason to upgrade is
if you build the Docker image or consume the drafted vcpkg port.

### Fixed

- **`docker build .` could not complete.** The builder stage never installed a
  libcurl development package, while `CMakeLists.txt` calls
  `find_package(CURL REQUIRED)`:

  ```
  CMake Error: Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)
  ```

  With that fixed the build reached the runtime stage and failed again, because
  nghttp3 and ngtcp2 install to the multiarch library directory on
  Debian-derived distributions rather than `/usr/lib`, so the `COPY` globs
  matched nothing:

  ```
  COPY failed: no source files were specified
  ```

  The corrected paths also pick up `libngtcp2_crypto_quictls`, which is needed
  at runtime and which the old globs missed as well.
- **The Docker build context was 3.1 GB.** `.dockerignore` covered four
  directories and missed `vcpkg/`, `vcpkg_installed/`, `build_cov/` and the
  other build trees, all of which were uploaded and baked into an image layer.
  Rewritten by category to match `.gitignore`, with an explicit block for
  credentials so key material cannot be captured in an image. The context is
  now 6.9 MB.
- The drafted vcpkg port's `SHA512` referred to the v1.4.0 archive while its
  manifest declared a newer version, so the port could not have verified its
  download.

### Changed

- The image no longer builds the test suite. It fetched GoogleTest over the
  network at configure time and compiled 18 translation units that nothing in
  the runtime stage uses.
- Removed the obsolete `version` key from `docker-compose.yml`, which Compose
  V2 warns about.

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

[v1.6.0]: https://github.com/varuns2903/orbit-framework/compare/v1.5.1...v1.6.0
[v1.5.1]: https://github.com/varuns2903/orbit-framework/compare/v1.5.0...v1.5.1
[v1.5.0]: https://github.com/varuns2903/orbit-framework/compare/v1.4.0...v1.5.0
[v1.4.0]: https://github.com/varuns2903/orbit-framework/compare/v1.3.0...v1.4.0
[v1.3.0]: https://github.com/varuns2903/orbit-framework/compare/v1.2.1...v1.3.0
[v1.2.1]: https://github.com/varuns2903/orbit-framework/compare/v1.2.0...v1.2.1
[v1.2.0]: https://github.com/varuns2903/orbit-framework/compare/v1.1.1...v1.2.0
[v1.1.1]: https://github.com/varuns2903/orbit-framework/compare/v1.1.0...v1.1.1
[v1.1.0]: https://github.com/varuns2903/orbit-framework/compare/v1.0.0...v1.1.0
[v1.0.0]: https://github.com/varuns2903/orbit-framework/releases/tag/v1.0.0
