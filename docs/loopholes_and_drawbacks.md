# Known Limitations

What to know before running Orbit in production. This page describes the
current state of `main`; it is updated when a limitation is fixed. For
security reporting see [SECURITY.md](../SECURITY.md), and for planned work
see [ROADMAP.md](ROADMAP.md).

## 1. HTTP/3 is experimental

The QUIC transport (ngtcp2/nghttp3) completes handshakes, but
`QuicHttp3Session` does not route requests yet: every HTTP/3 request gets a
fixed placeholder response, and file responses are not supported. Serve
production traffic over HTTP/1.1 or HTTP/2.

## 2. One event loop per `App`

Each `App` runs a single event-loop thread that accepts connections and
performs all socket I/O; handlers run on a worker thread pool
(`ServerConfig::worker_threads`). I/O-heavy workloads saturate that one
thread before the CPU does. To use more cores for I/O, run several processes
on the same port: the listener sets `SO_REUSEPORT` on Linux and macOS.

## 3. Large default dependency set

PostgreSQL, MariaDB, MongoDB, Redis and HTTP/3 support are enabled by
default. Turning one off (`-DORBIT_ENABLE_POSTGRES=OFF`, ...) removes it from
the Orbit build, but `vcpkg.json` still lists every backend's library, so
vcpkg installs them all on the first build (making them optional manifest
features is tracked in #36).

## 4. Custom protocol parsers

HTTP/1.1, multipart and WebSocket parsing are implemented in Orbit rather
than taken from a widely deployed library, which makes them the main attack
surface. Mitigations in place:

- the HTTP parser is fuzzed in CI (`.github/workflows/fuzz.yml`);
- WebSocket handling is checked against the Autobahn TestSuite
  (`.github/workflows/autobahn.yml`);
- the test suite runs under Valgrind in CI and under ASan/UBSan in local
  Debug builds;
- request framing follows RFC 9112 strictly (conflicting `Content-Length`,
  `Transfer-Encoding` with `Content-Length`, oversized headers and bodies
  are rejected), and every connection phase has a timeout.

Orbit has **not** had a third-party security audit or penetration test.

## 5. Asynchronous lifetimes

Handlers may finish after they return (database coroutines, thread-pool
work, SSE streams). Capture the `std::shared_ptr<ResponseWriter>` (and any
request data you need) by value in such callbacks; references to the
`HttpRequest` or to locals are dangling once the handler has returned.

## 6. Platform coverage is uneven

CI builds and tests every change on Ubuntu (epoll), macOS (kqueue) and
Windows (IOCP). Not covered by CI:

- the `io_uring` engine (`ServerConfig::engine = EventEngine::IoUring`),
  which is opt-in;
- HTTP/2 on Windows has one known intermittent test failure (#86).

## 7. No API or ABI stability yet

1.x is pre-stable: minor releases may change the API, and there is no ABI
guarantee — rebuild your application for each Orbit release. Every breaking
change is listed in [CHANGELOG.md](../CHANGELOG.md), with upgrade steps in
[migration.md](migration.md).

## 8. Test coverage

Line coverage is measured on every push and published in
[coverage.md](coverage.md). HTTP/1.1, routing, middleware and WebSockets are
the best exercised; HTTP/2, HTTP/3, the gRPC wrapper and parts of the ORM
have thinner coverage.

## 9. Swagger UI loads from a CDN

`App::enable_openapi()` serves a Swagger UI page that loads its scripts from
unpkg without Subresource Integrity (#41). Enable it only where you trust
that CDN, or not in production.
