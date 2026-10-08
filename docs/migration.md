# Migration Guide

Orbit 1.x is pre-stable: minor releases may break source compatibility (see
[API Stability](../README.md#-api-stability)). This guide lists every release that
needs changes in application code or build files, with before/after examples.
Releases that are not listed (1.1.x, 1.2.x, 1.4.0 → 1.5.1 patch) upgrade in place.

For the full list of changes see [CHANGELOG.md](../CHANGELOG.md).

## Applies to every version: `App` needs a `ServerConfig`

`server::App` has never had a default constructor. Older snippets that write
`server::App app;` do not compile.

```cpp
// Before (does not compile)
server::App app;

// After
config::ServerConfig config;
config.port = 8080;
server::App app(config);

// Or take the settings from the command line (--port, --bind, --threads, ...)
server::App app(config::ServerConfig::parse(argc, argv));
```

---

## 1.6.x → 2.0.0

### The public API is under `namespace orbit` ([#39](https://github.com/varuns2903/orbit-framework/issues/39))

Every type and function moved from generic top-level namespaces into
`orbit`: `server::App` is now `orbit::server::App`, `http::HttpRequest` is
`orbit::http::HttpRequest`, and so on for `concurrency`, `config`,
`database`, `http`, `middleware`, `network`, `openapi`, `orm`, `routing`,
`server`, `utils` and `websocket`.

**Existing code keeps compiling.** The old names are defined as aliases of
the new ones (`namespace server = orbit::server;`), so 1.x code builds
unchanged:

```cpp
// 1.x — still compiles in 2.0
server::App app(config);

// 2.0
orbit::server::App app(config);
```

The aliases put names like `server`, `http` and `config` in the global
namespace, which is what #39 set out to stop. If your own code uses any of
those names, define `ORBIT_NO_LEGACY_NAMESPACES` (before the first Orbit
include, or on the command line: `-DORBIT_NO_LEGACY_NAMESPACES`, or
`target_compile_definitions(app PRIVATE ORBIT_NO_LEGACY_NAMESPACES)`) and
use the `orbit::` names. The aliases will be removed in 3.0.

Code that refers to Orbit with a leading `::` (`::server::App`) needs the
new name; so does code that forward-declares Orbit types in the old
namespaces.

### HTTP/1.1 requests are parsed by llhttp

Request parsing moved to [llhttp](https://github.com/nodejs/llhttp)
([#43](https://github.com/varuns2903/orbit-framework/issues/43)), which is a
new build dependency (vcpkg, Conan and system packages are all supported).
It is strict, and a few requests are now answered differently:

- an unknown method token (`BREW / HTTP/1.1`) gets `501 Not Implemented`
  instead of being routed;
- a repeated `Content-Length` header is rejected with `400`, even when both
  values match;
- a request to a stream route without `Content-Length` or
  `Transfer-Encoding` has no body (RFC 9112): its stream ends at once
  instead of running until the client closes.

`http::parse_framing()`, `http::MessageFraming`, `http::decode_chunked()` and
`http::ChunkedStatus` were removed. `http::HttpParser::parse()` stays; it
now returns `std::nullopt` for an incomplete request instead of a partial
body.

### The gRPC wrapper is experimental

Built without gRPC (`ORBIT_ENABLE_GRPC=OFF`, the default),
`server::GrpcServer::start()` and `add_service()` now throw instead of doing
nothing, and `start()` throws when the server cannot start.

### New, opt-in

`ServerConfig::event_loops` (`--event-loops N`, Linux) runs several event
loops; the default of 1 keeps the previous behaviour.

## 1.5.x → 1.6.0

1.6.0 is a security release. Most changes are fixes, but several
intentionally tighten behaviour.

### Routing and requests

- **Wrong method → `405`.** A request whose path matches but whose method
  does not now gets `405 Method Not Allowed` with an `Allow` header (it was
  `404`). `HEAD` is answered from the `GET` route. Clients or tests that
  expected `404` need updating.
- **Decoded paths.** `req.uri`, `req.query` and route `params` are
  percent-decoded. Remove any manual decoding, or values are decoded twice.
  The raw request-target is still available as `req.target`.

  ```cpp
  // GET /files/my%20report.pdf?q=a%2Bb
  // Before: req.params["name"] == "my%20report.pdf", req.query["q"] == "a%2Bb"
  // After:  req.params["name"] == "my report.pdf",   req.query["q"] == "a+b"
  ```

  Paths containing an encoded `/`, `\` or NUL get `400`.
- **No exception text in 500s.** Uncaught handler exceptions return a generic
  `500`. To customise the response, or to log the exception, register
  `app.on_error(...)`. It now also receives exceptions from handlers that
  return a value.

### Middleware

- **CORS.** When an allow-list is configured, unlisted origins get no CORS
  headers at all (previously any origin was reflected).
- **JWT.** Only HS256 tokens are accepted; tokens with a future `nbf` are
  rejected; `jwt_auth("")` throws. Use `JwtOptions` for issuer/audience
  checks and leeway.
- **OAuth2.** Callbacks require the `state` cookie set by `login_handler()`.
  Start logins through `login_handler()` instead of redirecting to the
  provider yourself.
- **WebSockets.** WebSocket routes now run the app's middleware (auth, rate
  limiting, CORS/origin checks) before the upgrade. A middleware that rejects
  the request now also rejects the WebSocket.
- **Reverse proxy.** HTTPS upstreams are verified. For a private CA set
  `ca_file`; for a self-signed test upstream set `verify_tls = false`.

  ```cpp
  middleware::ProxyOptions opts;
  opts.ca_file = "/etc/ssl/internal-ca.pem";
  ```
- **Distributed rate limiter.** When Redis is unreachable, requests are now
  rejected with `503` (fail closed); before they got `429`. To let traffic
  through instead:

  ```cpp
  app.use(middleware::distributed_rate_limit("redis", 6379, 100, std::chrono::seconds(60),
                                             /*key=*/nullptr, /*allow_when_unavailable=*/true));
  ```
- **Static files.**
  - Dotfiles (`.env`, `.git/`) are no longer served; set
    `StaticFilesOptions::serve_dotfiles` if you really need them.
  - Text types now carry `; charset=utf-8`.
  - The ETag format changed, so browsers revalidate each cached file once.

### Database and ORM

- **Bound parameters.** The ORM sends values as bound parameters. Code that read `orm::Expr::sql`
  expecting values inlined, or passed arbitrary operator strings or
  non-identifier column names, must change. For raw SQL use the parameterised
  overloads:

  ```cpp
  // Before: values spliced into the SQL string (injectable)
  db.query("SELECT * FROM users WHERE email = '" + email + "'", cb);

  // After
  db.query("SELECT * FROM users WHERE email = $1", {email}, cb);
  ```
- **Failures are reported.** A failed `PostgresClient` query returns `ResultSet::failure(message)` instead of an
  empty result. Check it:

  ```cpp
  db.query(sql, [](const database::ResultSet& rs) {
      if (!rs.ok()) { LOG_ERROR(rs.error()); return; }
      // ...
  });
  ```
- **`to_json()`.** Empty strings stay `""`; only SQL NULL becomes `null`.

### Server configuration

- **Timeouts.** The single 10-second timer is replaced by per-phase timeouts. If you
  relied on connections closing after 10 s of activity, set them explicitly:

  ```cpp
  config.header_timeout = std::chrono::seconds(10);
  config.keep_alive_timeout = std::chrono::seconds(10);
  config.idle_timeout = std::chrono::seconds(30);
  config.websocket_idle_timeout = std::chrono::seconds(0); // 0 = never
  ```
- **Listen address and backlog.** New options `host` (`--bind`), `backlog` (`--backlog`, now `SOMAXCONN`
  by default instead of 10) and `max_connections`. The default bind address
  is unchanged (`0.0.0.0`).

### Build

- **hiredis.** The unused hiredis dependency was removed. If your own CMake
  linked `hiredis` only because Orbit required it, drop it.

---

## 1.4.0 → 1.5.0

Source compatible. `HttpResponse` setters now return the response, so they
can be chained; code that ignored the old `void` return still compiles.

```cpp
// Now possible
writer->send(HttpResponse().status(HttpStatus::Created).json(payload));
```

---

## 1.3.0 → 1.4.0

### CMake package and target renamed

The installed CMake package and its targets moved from `HttpServer` to
`OrbitFramework`.

```cmake
# Before
find_package(HttpServer REQUIRED)
target_link_libraries(my_app PRIVATE HttpServer::core)

# After
find_package(OrbitFramework REQUIRED)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```

### PostgreSQL callbacks receive a `ResultSet`

`PostgresClient::query` callbacks get a `database::ResultSet` instead of a
raw `PGresult*`, and no longer need to free it.

```cpp
// Before
db.query(sql, [](PGresult* res) {
    int rows = PQntuples(res);
    std::string name = PQgetvalue(res, 0, 0);
    PQclear(res);
});

// After
db.query(sql, [](const database::ResultSet& rs) {
    size_t rows = rs.size();
    std::string name = rs[0].get("name").value_or(""); // std::optional; empty for NULL
});
```

---

## 1.2.x → 1.3.0

### Headers moved under `orbit/`

Public headers moved from `src/` (installed as `include/http-server/`) to
`include/orbit/`. Update includes:

```cpp
// Before
#include "server/App.hpp"
#include "middleware/Cors.hpp"

// After
#include <orbit/server/App.hpp>
#include <orbit/middleware/Cors.hpp>
```

Add the install prefix's `include/` directory to your include path (the CMake
package does this for you) instead of `include/http-server/`.
