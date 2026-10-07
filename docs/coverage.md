# Test Coverage

This page records how much of Orbit the test suite actually exercises, so the
gaps are visible rather than assumed.

## Current figures

Produced by the [Code Coverage workflow](../.github/workflows/coverage.yml) on
2026-10-07 (commit `032130e`), with every subsystem enabled, over Orbit's own
sources only:

| Metric | Covered | Total | Percentage |
|--------|---------|-------|------------|
| Lines | 7,946 | 8,814 | **90.2%** |
| Functions | 1,075 | 1,137 | **94.5%** |
| Branches | 8,244 | 10,895 | **75.7%** |

The suite is about 590 tests. For comparison, on 2026-09-17 it covered 27.1% of
lines, 33.5% of functions and 13.7% of branches.

### Targets

| Metric | Target | Enforced? |
|--------|--------|-----------|
| Lines | 90% | No: a warning annotation when below, a notice when met |
| Branches | 75% | No: same |

Targets are informational. The coverage job reports them on every run (in its
summary and as an annotation) and never fails because of them; Codecov's
status checks are informational too. Raise a target when coverage has grown
past it, so a drop is noticed.

Branch coverage counts neither compiler-generated exception branches
(`--exclude-throw-branches`) nor branches gcov marks unreachable: in C++ almost
every call has a hidden unwinding branch, and counting those halved the figure
without describing any untested behaviour.

## What CI runs

The coverage job builds once with instrumentation and runs the suite twice:

1. On the default **epoll** engine.
2. On **io_uring** (`ORBIT_TEST_ENGINE=iouring`). Integration tests take their
   `ServerConfig` from `orbit::test::server_config()` in
   `tests/utils/TestConfig.hpp`, which follows this variable.

The counts from both runs go into one report.

The database client tests run against real servers:

| Database | How the server is provided |
|----------|----------------------------|
| PostgreSQL | started by the tests (`initdb`/`pg_ctl`) |
| Redis | started by the tests (`redis-server`) |
| MySQL | MariaDB 11 service container (`ORBIT_TEST_MYSQL_*`) |
| MongoDB | MongoDB 7 service container (`ORBIT_TEST_MONGODB_URI`) |

Locally, a test whose server is missing **skips**. In the coverage job
`ORBIT_REQUIRE_{POSTGRES,REDIS,MYSQL,MONGODB}_TESTS` are set, so a server that
fails to start **fails** the run instead of quietly lowering coverage.

## Reproducing this

```bash
cmake -B build_cov -S . \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DORBIT_ENABLE_COVERAGE=ON \
  -DENABLE_SANITIZERS=OFF \
  -DORBIT_BUILD_EXAMPLES=OFF \
  -DORBIT_ENABLE_GRPC=OFF

cmake --build build_cov --parallel
cd build_cov

# Optional: servers for the MySQL and MongoDB tests (others skip without them).
docker run -d --rm -p 3306:3306 -e MARIADB_ROOT_PASSWORD=orbit -e MARIADB_DATABASE=orbit_test mariadb:11
docker run -d --rm -p 27017:27017 mongo:7
export ORBIT_TEST_MYSQL_PORT=3306 ORBIT_TEST_MYSQL_PASSWORD=orbit
export ORBIT_TEST_MONGODB_URI=mongodb://127.0.0.1:27017

ctest
ORBIT_TEST_ENGINE=iouring ctest

gcovr -r .. . \
  --filter "$(cd .. && pwd)/src/" \
  --filter "$(cd .. && pwd)/include/orbit/" \
  --exclude '.*json\.hpp' \
  --exclude-throw-branches \
  --exclude-unreachable-branches \
  --html-details coverage.html \
  --print-summary
```

Sanitizers are disabled because they interfere with gcov instrumentation.

The filters matter. Allow-listing `src/` and `include/orbit/` is deliberate:
an exclude list looks equivalent but quietly lets libstdc++ headers, pulled in
through templates, into the denominator. `include/orbit/http/json.hpp` is then
dropped explicitly, because it sits inside the allow-listed tree but is a
24,765-line vendored copy of nlohmann/json that would dominate the result.

A file compiled out of the build is not in the denominator at all: measuring
with `ORBIT_ENABLE_REDIS=OFF` drops `RedisClient.cpp` and *raises* the
percentage without a single new test. Use the flags above for a comparable
number.

## Where the gaps are

Most uncovered lines, from the run above:

| File | Lines | Covered |
|------|------:|--------:|
| `src/middleware/Proxy.cpp` | 513 | 78% |
| `src/server/Connection.cpp` | 795 | 86% |
| `src/server/QuicConnection.cpp` | 297 | 80% |
| `src/server/App.cpp` | 233 | 81% |
| `src/middleware/OAuth2.cpp` | 183 | 77% |
| `src/database/PostgresClient.cpp` | 188 | 81% |
| `src/utils/Logger.cpp` | 103 | 78% |

Most of what remains in `App.cpp` is `hot_reload()`, which forks and
re-executes the running process and so is not unit-testable as written. The
HTML report attached to each coverage run shows the uncovered lines of every
file.

Files that were entirely untested on 2026-09-17:

| File | Then | Now |
|------|-----:|----:|
| `src/network/IoUringProactor.cpp` | 0% | 84% |
| `src/database/RedisClient.cpp` | 0% | 91% |
| `src/database/MysqlClient.cpp` | 0% | 87% |
| `src/database/MongoClient.cpp` | 0% | 98% |
| `src/middleware/Csrf.cpp` | 0% | 100% |
| `src/config/Config.cpp` | 0% | 100% |
| `src/network/ConnectionPool.cpp` | 0% | 94% |
| `src/openapi/OpenApi.cpp` | 3% | 98% |

Writing these tests found and fixed real bugs, among them: a MongoDB client
created after another was destroyed crashed the process; Redis `get()` reported
a key holding `""` as missing; `/swagger.json` was invalid JSON for any route
with an inline response schema; `--port 70000` silently listened on 4464;
`/metrics` rounded every value past a million; and io_uring leaked every
request still in flight at shutdown.

## Contributing tests

- Assert observable behaviour. Placeholder tests such as `EXPECT_TRUE(true)`
  are not accepted (see `CONTRIBUTING.md`).
- Keep tests hermetic: no external network. A service a test needs is started
  by the test or by CI, and the test skips locally without it.
- New integration tests should build their `ServerConfig` from
  `orbit::test::server_config()` so they run on both event engines.
- Tests that also build on Windows must not include `<future>` or POSIX-only
  headers such as `<unistd.h>`: on MSVC `<future>` declares a `concurrency`
  namespace that collides with Orbit's.
