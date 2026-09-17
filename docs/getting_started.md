# Getting Started with Orbit

Orbit is a high-performance C++20 HTTP/3 web framework built on top of asynchronous kernel event loops (`io_uring` and `epoll`). It aims to provide Express.js-like ergonomics with raw C++ performance.

## Prerequisites

- **C++20 Compiler**: GCC 11+ or Clang 14+
- **Linux Kernel 5.6+**: Required for `io_uring` support (will fallback to `epoll` on older kernels).
- **CMake 3.15+**
- **Libraries**: OpenSSL, liburing, hiredis, libpq

## Installation & Build

We highly recommend using `vcpkg` to automatically install all required dependencies (like OpenSSL, PostgreSQL, MongoDB, ngtcp2, etc.) so you don't have to compile them from source.

```bash
git clone https://github.com/varuns2903/orbit-framework.git orbit
cd orbit

# Clone vcpkg if you don't have it installed
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# Build Orbit Framework with vcpkg toolchain
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Build with Conan (Alternative to vcpkg)

If you prefer Conan 2.x for dependency management:

```bash
# Install dependencies using the provided conanfile.py
conan install . --output-folder=build --build=missing

# Build the framework
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Manual Build (Without vcpkg)

If you prefer using system packages, install the prerequisites manually:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Integrating via CMake FetchContent (Recommended)

If you have your own CMake project and want to include Orbit seamlessly without manually building it first, you can use CMake's `FetchContent`. Since Orbit manages its internal examples and tests safely, fetching it will only build the core library.

Add this to your `CMakeLists.txt`:

```cmake
include(FetchContent)
FetchContent_Declare(
  OrbitFramework
  GIT_REPOSITORY https://github.com/varuns2903/orbit-framework.git
  GIT_TAG        main # Pin a release tag once one carries the current fixes
)
FetchContent_MakeAvailable(OrbitFramework)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE OrbitFramework::core)
```
*(Make sure to still pass `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake` when building your own project so the dependencies resolve).*

## Your First Orbit Server

Create a `main.cpp` file:

```cpp
#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/json.hpp>
#include <iostream>

int main() {
    config::ServerConfig config;
    config.port = 8080;

    server::App app(config);

    // Return a string — Orbit builds the HTTP response for you
    app.get("/", []() -> std::string {
        return "Hello from Orbit!";
    });

    // Return JSON
    app.get("/api/status", []() -> nlohmann::json {
        return {{"status", "ok"}};
    });

    std::cout << "Server starting on port 8080\n";
    app.listen();

    return 0;
}
```

Build it against Orbit (see the [Installation section](../README.md#-installation)
for every available method) and test:

```bash
curl http://localhost:8080
curl http://localhost:8080/api/status
```
