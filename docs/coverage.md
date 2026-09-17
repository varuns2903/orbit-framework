# Test Coverage

Orbit's roadmap targets 85-90% line coverage. This page records where the
project actually is, so the gap is visible rather than assumed.

## Current baseline

Measured on 2026-09-17 against commit `6088bcd`, over Orbit's own sources
(third-party, tests, and examples excluded):

| Metric | Covered | Total | Percentage |
|--------|---------|-------|------------|
| Lines | 1,180 | 4,259 | **27.7%** |
| Functions | 173 | 489 | **35.4%** |
| Branches | 963 | 6,854 | **14.1%** |

The suite is 101 tests and they all pass — but passing tests and covered code
are different things, and the second number is the one that says how much of
Orbit is actually exercised.

## Reproducing this

```bash
cmake -B build_cov -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DORBIT_ENABLE_COVERAGE=ON \
  -DENABLE_SANITIZERS=OFF \
  -DORBIT_BUILD_EXAMPLES=OFF

cmake --build build_cov --parallel
cd build_cov && ctest

gcovr -r .. . \
  --exclude '.*/_deps/.*' \
  --exclude '.*/tests/.*' \
  --exclude '.*json\.hpp' \
  --exclude '.*/examples/.*' \
  --html-details coverage.html \
  --print-summary
```

Sanitizers are disabled because they interfere with gcov instrumentation.
The exclusions matter: `_deps/` holds GoogleTest and inja, and
`include/orbit/http/json.hpp` is a 24,765-line vendored copy of nlohmann/json.
Counting those would produce a flattering number that says nothing about Orbit.

The [Code Coverage workflow](../.github/workflows/coverage.yml) runs this on
every push to `main` and attaches the HTML report as a build artifact.

## Where the gaps are

Entirely uncovered, ordered by size — these are where contribution has the most
effect:

| File | Lines | Covered |
|------|-------|---------|
| `src/server/QuicConnection.cpp` | 170 | 0% |
| `src/network/IoUringProactor.cpp` | 154 | 0% |
| `src/server/QuicHttp3Session.cpp` | 119 | 0% |
| `src/database/MysqlClient.cpp` | 104 | 0% |
| `src/database/PostgresClient.cpp` | 98 | 0% |
| `src/middleware/OAuth2.cpp` | 94 | 0% |
| `src/database/MongoClient.cpp` | 78 | 0% |
| `src/middleware/Csrf.cpp` | 63 | 0% |
| `src/http/MultipartForm.cpp` | 58 | 0% |
| `src/middleware/StaticFiles.cpp` | 48 | 0% |
| `src/server/QuicConnectionManager.cpp` | 46 | 0% |
| `src/config/Config.cpp` | 44 | 0% |
| `src/network/ConnectionPool.cpp` | 43 | 0% |
| `src/openapi/OpenApi.cpp` | 124 | 3.2% |

Best covered today:

| File | Lines | Covered |
|------|-------|---------|
| `src/http/MultipartStreamParser.cpp` | 112 | 83.9% |
| `include/orbit/database/ConnectionPool.hpp` | 45 | 80.0% |
| `src/http/HttpParser.cpp` | 74 | 75.7% |
| `src/network/EpollProactor.cpp` | 186 | 59.1% |
| `src/server/EventLoop.cpp` | 62 | 56.5% |
| `src/routing/Router.cpp` | 207 | 50.7% |

## Reading these numbers fairly

Some of the zeroes are easier to fix than others:

- **Database clients** need a live server. Either integration tests with
  service containers in CI, or a fake at the `ResultSet` boundary.
- **QUIC and io_uring** need real sockets and kernel support. The tractable
  first step is extracting pure logic and testing that, which is how the
  WebSocket framing and HTTP/2 header tests were built — see
  [#14](https://github.com/varuns2903/orbit-framework/issues/14).
- **Middleware** (`Csrf`, `OAuth2`, `StaticFiles`) is mostly pure request and
  response handling and needs no I/O. These are the cheapest wins on this page.
- **`Config.cpp` and `MultipartForm.cpp`** are self-contained parsing and are
  straightforward to test directly.

Also note that a file compiled out of the build does not appear in the
denominator at all. Run the measurement with the same feature flags you intend
to compare against, or the totals will move for reasons unrelated to tests.

Tracked in [#21](https://github.com/varuns2903/orbit-framework/issues/21).
