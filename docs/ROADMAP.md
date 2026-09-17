# Orbit Framework - Master Roadmap

This document outlines the goals and roadmap for the Orbit Framework to evolve into a production-ready, widely adopted ecosystem.

Unchecked items are genuinely unfinished, and checked items with a note attached
have a caveat worth reading. Anything marked **Help wanted** links to an open
issue you can pick up — see also the
[good first issues](https://github.com/varuns2903/orbit-framework/labels/good%20first%20issue)
and [CONTRIBUTING.md](../CONTRIBUTING.md).


## 1. Build System & Packaging
- [x] **1. Use CMake as the primary build system**
- [x] **2. Provide clean `find_package()` support**
- [x] **3. Export CMake targets** (e.g., `OrbitFramework::core`)
- [x] **4. Support CMake `FetchContent`** seamlessly
- [ ] **5. Publish to vcpkg** — a registry port is drafted at `packaging/vcpkg-port/`, but it has not been submitted to the upstream vcpkg registry. `vcpkg install orbit-framework` does not work yet. Consuming Orbit through vcpkg *manifest mode* (the root `vcpkg.json`) does work. **Help wanted: [#20](https://github.com/varuns2903/orbit-framework/issues/20).**
- [ ] **6. Publish to Conan** — `conanfile.py` builds Orbit locally via `conan install`/`conan create`, but the package has not been submitted to ConanCenter. **Help wanted.**
- [x] **7. Automatically manage dependencies**
- [x] **18. Provide prebuilt binaries/releases**
- [x] **20. Support both static and shared libraries**
- [x] **42. Keep core dependencies lightweight**
- [x] **43. Avoid forcing unused features/dependencies on users**

## 2. Platform & Modularity
- [x] **8. Make features modular and optional** — both the library and the examples honour the `ORBIT_ENABLE_*` flags; disabling a subsystem builds fewer examples rather than failing to link.
- [x] **19. Support Linux, Windows, and macOS** 
- [x] **9. HTTP/1.1**
- [x] **10. HTTP/2**
- [x] **11. HTTP/3** (QUIC)
- [x] **12. REST** (Router & Middleware)
- [x] **56. Magic Return Values & Auto-JSON HTTP Handlers (FastAPI-style)**
- [x] **13. WebSockets**
- [x] **55. Strongly-Typed WebSocket EventRouter (Auto-JSON mapping, Rooms & Session State)**
- [x] **14. GraphQL** (HTTP adapter middleware)
- [x] **15. gRPC** (GrpcServer wrapper) — built only with `ORBIT_ENABLE_GRPC=ON`, which CI does not exercise; support level under discussion in [#22](https://github.com/varuns2903/orbit-framework/issues/22).
- [x] **16. TLS/SSL** (OpenSSL integration)

## 2.5 Database & ORM
- [x] **51. Database Abstraction Layer (DBAL) & unified ResultSet**
- [x] **52. Automatic JSON serialization for database queries**
- [x] **53. Full Object-Relational Mapper (ORM)**
- [x] **54. Database Migrations support**

## 3. Tooling & Developer Experience
- [x] **21. Provide a CLI** (`orbit-cli`)
- [x] **22. `orbit new <project>`**
- [x] **23. `orbit build`**
- [x] **24. `orbit run`**
- [x] **17. Provide sensible default configuration**
- [x] **44. Provide one-command installation**
- [x] **45. Provide one-command project creation**
- [x] **46. Provide one-command development run**
- [x] **47. Provide one-command release/production build**
- [x] **50. Build the developer experience around: install → create → code → run → deploy**

## 4. Documentation & Education
- [x] **25. Provide project templates/scaffolding**
- [x] **26. Provide a 5-minute Hello World example**
- [x] **27. Provide examples for every major feature**
- [x] **28. Provide Docker support**
- [x] **29. Provide production deployment documentation**
- [x] **32. Provide API documentation**
- [ ] **33. Provide migration guides** — **Help wanted: [#18](https://github.com/varuns2903/orbit-framework/issues/18).**
- [x] **48. Provide clear README: Install → Create → Run → Deploy**
- [ ] **49. Provide comprehensive documentation**

## 5. Testing & CI/CD
- [x] **35. Automated CI/CD**
- [x] **36. Test Linux, Windows and macOS**
- [x] **57. Code Coverage Setup (Lcov/Gcovr & CMake Integration)** — the CMake wiring exists; no figure is published yet, see [#21](https://github.com/varuns2903/orbit-framework/issues/21).
- [ ] **37. Unit Testing Suite Expansion (Hit 85-90% Target)** — in progress; the suite covers routing, parsing, middleware, ORM, WebSocket framing, and HTTP/2 header encoding, but is well short of 85-90% line coverage. The figure has not been measured — see [#21](https://github.com/varuns2903/orbit-framework/issues/21). **Help wanted: [#14](https://github.com/varuns2903/orbit-framework/issues/14) (QUIC), [#15](https://github.com/varuns2903/orbit-framework/issues/15) (fuzzing).**
- [ ] **38. Integration Testing Suite Expansion** — **Help wanted: [#16](https://github.com/varuns2903/orbit-framework/issues/16).**
- [ ] **39. HTTP/protocol compliance tests** — **Help wanted: [#17](https://github.com/varuns2903/orbit-framework/issues/17).**
- [x] **40. Load Testing & Benchmarking** — ApacheBench figures are in [benchmarks.md](benchmarks.md); no comparison against other frameworks yet, see [#19](https://github.com/varuns2903/orbit-framework/issues/19).
- [ ] **41. Attack / Penetration Testing** — the HTTP parser is fuzzed nightly in CI and the suite runs under ASan/UBSan plus Valgrind, but Orbit has had no third-party security audit or structured penetration test. See [loopholes_and_drawbacks.md](loopholes_and_drawbacks.md) and [SECURITY.md](../SECURITY.md). **Help wanted.**

## 6. Maintenance & Lifespan
- [x] **30. Use semantic versioning**
- [ ] **31. Maintain API/ABI compatibility where possible** — not yet guaranteed. The README's [API Stability](../README.md#-api-stability) section states this explicitly so that semver 1.x is not read as a stability promise Orbit does not currently make.
- [x] **34. Maintain a detailed changelog**
