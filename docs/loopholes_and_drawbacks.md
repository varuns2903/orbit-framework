# Loopholes and Drawbacks

While Orbit is a highly performant and modern framework, its architecture and current state have several potential loopholes and drawbacks that must be addressed before production use.

## 1. Complex Dependency Management
The framework relies heavily on a multitude of system-installed C++ libraries:
- OpenSSL
- PostgreSQL (`libpq`), MariaDB, MongoDB drivers
- `ngtcp2`, `nghttp3` (for HTTP/3)
- `liburing`
Relying entirely on system-level `find_package` without a unified package manager makes it incredibly difficult for new users to compile the project across different machines and operating systems.

## 2. Security Surface Area
- **Custom Parsing**: Orbit implements its own custom parsers for HTTP headers, Multipart forms, and integrates heavily with complex protocols like QUIC. This opens up a massive attack surface for vulnerabilities such as buffer overflows, out-of-bounds reads, or Slowloris attacks.
- **Raw Socket Handling**: Interfacing directly with `io_uring`, `epoll`, and `kqueue` means memory mismanagement can easily lead to catastrophic server crashes. Extensive security auditing and fuzzing are required.

## 3. C++20 Coroutine Pitfalls
Orbit extensively uses C++20 coroutines for non-blocking I/O (like database queries). While syntactically elegant, coroutines in C++ are notorious for tricky lifetime management.
- **Use-After-Free Risks**: If a request context (like the HTTP response object) is destroyed before a suspended database coroutine resumes, the server will encounter a segmentation fault. Strict lifetime tracking and memory ownership (via `std::shared_ptr` or arena allocators) must be enforced.

## 4. Platform Support Ambiguity
- The project claims "True Cross-Platform Portability" and includes code for Windows `IOCP` (`IocpProactor.cpp`). However, system requirements only mention Linux `io_uring`. 
- **Missing CI**: There is no automated Cross-Platform Continuous Integration (CI). Without CI testing Windows, macOS, and Linux on every commit, platform-specific code will inevitably break and regress silently.
