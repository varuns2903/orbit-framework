# Test Coverage

Orbit's roadmap targets 85-90% line coverage. This page records where the
project actually is, so the gap is visible rather than assumed.

## Current baseline

Produced by the [Code Coverage workflow](../.github/workflows/coverage.yml) on
2026-09-17, with every subsystem enabled, over Orbit's own sources only:

| Metric | Covered | Total | Percentage |
|--------|---------|-------|------------|
| Lines | 1,184 | 4,362 | **27.1%** |
| Functions | 162 | 484 | **33.5%** |
| Branches | 974 | 7,104 | **13.7%** |

The suite is 109 tests and they all pass — but passing tests and covered code
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
  --filter "$(cd .. && pwd)/src/" \
  --filter "$(cd .. && pwd)/include/orbit/" \
  --exclude '.*json\.hpp' \
  --html-details coverage.html \
  --print-summary
```

Sanitizers are disabled because they interfere with gcov instrumentation.

The filters matter. Allow-listing `src/` and `include/orbit/` is deliberate:
an exclude list looks equivalent but quietly lets libstdc++ headers, pulled in
through templates, into the denominator. `include/orbit/http/json.hpp` is then
dropped explicitly, because it sits inside the allow-listed tree but is a
24,765-line vendored copy of nlohmann/json that would dominate the result.

The [Code Coverage workflow](../.github/workflows/coverage.yml) runs this on
every push to `main` and attaches the HTML report as a build artifact.

## Where the gaps are

Entirely uncovered, ordered by size — these are where contribution has the most
effect:

| File | Lines | Covered |
|------|-------|---------|
| `src/server/QuicConnection.cpp` | 170 | 0% |
| `src/network/IoUringProactor.cpp` | 152 | 0% |
| `src/server/QuicHttp3Session.cpp` | 118 | 0% |
| `src/database/RedisClient.cpp` | 111 | 0% |
| `src/database/MysqlClient.cpp` | 104 | 0% |
| `src/database/PostgresClient.cpp` | 98 | 0% |
| `src/middleware/OAuth2.cpp` | 94 | 0% |
| `src/database/MongoClient.cpp` | 78 | 0% |
| `src/middleware/Csrf.cpp` | 63 | 0% |
| `src/http/MultipartForm.cpp` | 58 | 0% |
| `src/middleware/StaticFiles.cpp` | 48 | 0% |
| `src/server/QuicConnectionManager.cpp` | 46 | 0% |
| `src/config/Config.cpp` | 44 | 0% |
| `src/network/ConnectionPool.cpp` | 44 | 0% |

`src/openapi/OpenApi.cpp` (124 lines) sits just above zero at 3%.

Best covered today:

| File | Lines | Covered |
|------|-------|---------|
| `src/http/MultipartStreamParser.cpp` | 112 | 83% |
| `src/http/HttpParser.cpp` | 93 | 78% |
| `src/network/EpollProactor.cpp` | 184 | 58% |
| `src/server/EventLoop.cpp` | 62 | 56% |
| `src/routing/Router.cpp` | 207 | 50% |
| `src/http/HttpResponse.cpp` | 96 | 50% |

`src/server/Connection.cpp` deserves its own mention: at 593 lines it is the
largest file in the project and sits at 33%, so it contributes more uncovered
lines than any other single file.

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
denominator at all — measuring with `ORBIT_ENABLE_REDIS=OFF` drops
`RedisClient.cpp` and its 111 uncovered lines, which *raises* the percentage
without a single new test. The figures above come from a CI build with every
subsystem enabled, so use the same flags if you want a comparable number.

Tracked in [#21](https://github.com/varuns2903/orbit-framework/issues/21).
