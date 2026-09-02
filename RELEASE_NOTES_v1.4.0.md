# Orbit v1.4.0 Release Notes

We are thrilled to announce Orbit v1.4.0, a massive update that dramatically enhances developer experience, adds powerful new protocol support, overhauls the build/packaging system, and establishes a robust native C++ test suite!

## 🚀 Key Features

* **Magic Return Values (FastAPI-style):** Route handlers can now directly return native types (like `nlohmann::json`, `std::string`, structs) and Orbit will automatically serialize them and construct the HTTP response! 
* **Socket.IO-style WebSocket EventRouter:** We introduced a strongly-typed, intuitive `EventRouter` for WebSockets. It fully supports custom events, data payloads, rooms, and sessions.
* **Unified DBAL & ORM:** A brand new Expression Template-based ORM Query DSL. Write SQL safely and efficiently in native C++. Also includes new MongoDB ORM integration alongside PostgreSQL.
* **GraphQL & gRPC Adapters:** Added a new GraphQL middleware adapter and an optional gRPC server wrapper to extend Orbit beyond traditional REST.
* **Orbit CLI:** A brand new command-line tool `orbit` to effortlessly scaffold and manage new Orbit projects.

## 📦 Build & Packaging Enhancements

* **Modular CMake & Build Options:** Control exactly what gets built with new `ORBIT_ENABLE_xxx` flags (e.g. toggle gRPC, QUIC/HTTP3).
* **Cross-Platform & CI:** Added comprehensive Windows support to GitHub Actions.
* **Package Managers:** Full support for `vcpkg`, `Conan`, and CMake `FetchContent`.
* **Distribution:** Added `CPack` support for generating installable release packages and support for `BUILD_SHARED_LIBS`.

## 🧪 Testing & Reliability

* **Pure Native C++ Test Suite:** Migrated our entire testing infrastructure from Python scripts to a high-performance native C++ suite using GoogleTest.
* **Comprehensive Code Coverage:** Integrated `gcovr` with a new fully functional C++ E2E HTTP integration runner and unit test suite.
* **Bug Fixes:**
  - Fixed a critical thread deadlock vulnerability in `ConnectionPool`.
  - Fixed HandlerWrapper deduction failures for Coroutine tasks modifying the ResponseWriter.
  - Addressed various target linkage and export issues for consumers.

Thanks to all contributors for pushing Orbit forward!
