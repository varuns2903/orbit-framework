# Contributing to Orbit

Thanks for taking the time to contribute. Orbit is a C++20 web framework, and
this guide covers everything you need to get a patch merged.

## Table of Contents

- [Project status](#project-status)
- [Ways to contribute](#ways-to-contribute)
- [Development setup](#development-setup)
- [Building](#building)
- [Running the tests](#running-the-tests)
- [Coding standards](#coding-standards)
- [Commit messages](#commit-messages)
- [Pull request process](#pull-request-process)
- [Reporting bugs](#reporting-bugs)
- [Reporting security issues](#reporting-security-issues)

## Project status

Orbit is young and actively developed. The public API is **not frozen** — see
the API stability note in the [README](README.md#-api-stability). Breaking
changes are possible in minor releases and are documented in
[CHANGELOG.md](CHANGELOG.md).

Areas that most need help are tracked in [docs/ROADMAP.md](docs/ROADMAP.md).
Anything unchecked there is fair game.

## Ways to contribute

You do not need to write C++ to help:

- **Test coverage** — several subsystems (HTTP/2, WebSocket framing, QUIC) have
  thin unit tests. This is the highest-impact area right now.
- **Documentation** — the guides in `docs/` always need corrections and examples.
- **Examples** — `examples/` holds one file per feature; new realistic ones are welcome.
- **Bug reports** — a reproducible report is a real contribution.
- **Platform testing** — macOS and Windows get less real-world use than Linux.

If you are looking for a starting point, issues labelled `good first issue` are
scoped to be completable without deep knowledge of the codebase.

## Development setup

### Prerequisites

- A C++20 compiler — GCC 11+ or Clang 14+ (MSVC 19.30+ on Windows)
- CMake 3.20+
- Git
- On Linux, kernel 5.6+ to exercise the `io_uring` backend (Orbit falls back to
  `epoll` on older kernels)

### Dependencies

Orbit depends on OpenSSL, zlib, libpq, curl, libmariadb, mongo-c-driver,
nghttp2, nghttp3, ngtcp2, hiredis, and (on Linux) liburing. Do not install these
by hand — use vcpkg, which reads the dependency list from `vcpkg.json`:

```bash
git clone https://github.com/varuns2903/orbit-framework.git
cd orbit-framework

git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh      # bootstrap-vcpkg.bat on Windows
```

The first configure builds every dependency from source and can take 20-40
minutes. Subsequent configures reuse the vcpkg binary cache.

## Building

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug \
  -DORBIT_BUILD_TESTS=ON

cmake --build build --parallel
```

### Useful CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `ORBIT_BUILD_TESTS` | `ON` | Build the GoogleTest suite |
| `ORBIT_ENABLE_HTTP3` | `ON` | HTTP/3 and QUIC support |
| `ORBIT_ENABLE_REDIS` | `ON` | Redis client |
| `ORBIT_ENABLE_POSTGRES` | `ON` | PostgreSQL client |
| `ORBIT_ENABLE_MARIADB` | `ON` | MySQL/MariaDB client |
| `ORBIT_ENABLE_MONGODB` | `ON` | MongoDB client |
| `ORBIT_ENABLE_GRPC` | `OFF` | gRPC server wrapper |
| `ENABLE_SANITIZERS` | `ON` | ASan + UBSan |
| `ORBIT_ENABLE_COVERAGE` | `OFF` | gcov/lcov instrumentation |

Turning off subsystems you are not touching makes builds substantially faster.

## Running the tests

```bash
cd build
ctest --output-on-failure
```

Every test must pass before you open a pull request. CI runs the same suite on
Ubuntu, macOS, and Windows.

### Sanitizers

`ENABLE_SANITIZERS=ON` (the default) builds with AddressSanitizer and
UndefinedBehaviorSanitizer. Because Orbit does raw socket and buffer handling,
please run the suite under sanitizers before submitting changes to the event
loop, parsers, or QUIC code.

### Memory leaks

CI additionally runs Valgrind on Linux:

```bash
valgrind --leak-check=full --error-exitcode=1 ./build/http_server_tests
```

### Fuzzing

`tests/fuzz/` holds libFuzzer targets for the HTTP parser. If you change a
parser, extend the corresponding fuzz target:

```bash
cmake -B build_fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build_fuzz --target fuzz_http_parser
./build_fuzz/fuzz_http_parser -max_total_time=60
```

### Writing tests

Tests live in `tests/unit/`, `tests/integration/`, and `tests/fuzz/`. New unit
test files are picked up automatically by the glob in `CMakeLists.txt`.

A test must actually assert something about behaviour. Placeholder bodies such
as `EXPECT_TRUE(true)` are not accepted — if a component is hard to test in
isolation, extract the pure logic (framing, parsing, state transitions) into a
testable function rather than asserting a tautology.

## Coding standards

Orbit has no automated formatter yet, so match the surrounding file:

- 4-space indent, no tabs.
- `snake_case` for functions, variables, and members; `PascalCase` for types;
  `UPPER_SNAKE_CASE` for macros and constants.
- One namespace per subsystem (`http`, `network`, `database`, `middleware`,
  `server`, `config`), matching the directory layout under `include/orbit/`.
- Public headers go in `include/orbit/<subsystem>/`, implementation in
  `src/<subsystem>/`. Header-only middleware lives in
  `include/orbit/middleware/`.
- Prefer `std::string_view` and spans over raw pointer/length pairs.
- Every public API gets a Doxygen `@brief`; the docs site is generated from
  these comments.
- No new third-party dependency without discussion in an issue first. If one is
  unavoidable it must be declared in `vcpkg.json` and `conanfile.py`, and
  guarded behind a CMake option if it is not core.

### Memory and lifetime

Orbit uses C++20 coroutines over raw kernel I/O, which makes lifetime bugs
easy to introduce and hard to spot. When a coroutine suspends across an I/O
boundary, anything it holds by reference must outlive the resumption — capture
`std::shared_ptr` rather than references in async callbacks. Changes in this
area will be reviewed closely and must come with sanitizer output.

## Commit messages

Orbit uses [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<optional scope>): <subject>

<optional body explaining why, not what>
```

Types in use: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci`,
`chore`.

Keep the subject under 72 characters and in the imperative mood. Example:

```
fix(http2): reject CONTINUATION frames after END_HEADERS

A malformed peer could desynchronise the HPACK decoder by interleaving
frames, leaving the session unable to parse subsequent requests.
```

## Pull request process

1. Fork the repository and branch from `main`.
2. Make your change, with tests.
3. Run the full suite locally (`ctest --output-on-failure`).
4. Update `docs/` if you changed behaviour, and add a `CHANGELOG.md` entry
   under `## [Unreleased]` if the change is user-visible.
5. Open the pull request using the template, describing what changed and why.
6. Ensure CI is green on all three platforms. Maintainers will not merge a red
   build.

Keep pull requests focused. A PR that fixes one bug is reviewed in a day; a PR
that fixes one bug and reformats four files is reviewed in a month.

## Reporting bugs

Open an issue using the bug report template. A good report includes the Orbit
version or commit, OS and kernel version, compiler and version, the CMake
options used, and a minimal `main.cpp` that reproduces the problem.

## Reporting security issues

**Do not open a public issue for a security vulnerability.** See
[SECURITY.md](SECURITY.md) for the disclosure process.

## License

By contributing, you agree that your contributions are licensed under the MIT
License that covers this project.
