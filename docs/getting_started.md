# Getting Started with Orbit

Orbit is a high-performance C++20 HTTP/3 web framework built on top of asynchronous kernel event loops (`io_uring` and `epoll`). It aims to provide Express.js-like ergonomics with raw C++ performance.

## Prerequisites

- **C++20 Compiler**: GCC 11+ or Clang 14+
- **Linux Kernel 5.6+**: Required for `io_uring` support (will fallback to `epoll` on older kernels).
- **CMake 3.15+**
- **Libraries**: OpenSSL, liburing, hiredis, libpq

## Installation & Build

Clone the repository and build using CMake:

```bash
git clone https://github.com/varuns2903/orbit-framework.git orbit
cd orbit
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Your First Orbit Server

Create a `main.cpp` file:

```cpp
#include "server/App.hpp"
#include "http/HttpResponse.hpp"
#include <iostream>

using namespace server;
using namespace http;

int main() {
    App app;

    // Define a simple GET route
    app.get("/", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().status(HttpStatus::OK).send("Hello from Orbit!"));
    });

    // Start the server on port 8080
    app.listen(8080, []() {
        std::cout << "Server started on port 8080!" << std::endl;
    });

    return 0;
}
```

Compile it and link against `libserver_core.a`. Run it and test:
```bash
curl http://localhost:8080
```
