# Third-Party Notices

Orbit Framework is distributed under the [MIT License](LICENSE). It bundles,
fetches, and links third-party components that carry their own licenses and
copyright notices, reproduced or referenced below.

This file covers the default build. Components tied to an `ORBIT_ENABLE_*`
option are only involved when that option is on.

---

## Bundled in this repository

Source shipped inside the Orbit source tree.

### JSON for Modern C++ (nlohmann/json)

- **Version:** 3.11.3
- **Location in this repository:** `include/orbit/http/json.hpp`
- **Upstream:** https://github.com/nlohmann/json
- **License:** MIT
- **Copyright:** SPDX-FileCopyrightText: 2013-2023 Niels Lohmann <https://nlohmann.me>

The upstream single-header amalgamation is vendored verbatim, with its SPDX
copyright and license identifiers intact at the top of the file. Orbit's public
API exposes `nlohmann::json` directly — handlers may return it, and the ORM
serialises rows through it.

> **Note for consumers.** Because this header defines `nlohmann::json` itself,
> linking Orbit into a project that also pulls in its own copy of
> nlohmann/json can put two different versions of those symbols in one program.
> The library is header-only and its symbols are inline, but mixing versions
> across translation units is still undefined behaviour. If your project
> already uses nlohmann/json, prefer including Orbit's copy
> (`<orbit/http/json.hpp>`) rather than adding a second one, or pin your
> version to 3.11.3.

---

## Fetched at configure time

Downloaded by CMake `FetchContent` during configuration; not redistributed in
this repository.

### inja

- **Version:** v3.4.0
- **Upstream:** https://github.com/pantor/inja
- **License:** MIT
- **Used for:** server-side template rendering in `src/http/HttpResponse.cpp`

inja is linked `PRIVATE` and appears in no public Orbit header, so it is not
part of the installed package's link interface.

### GoogleTest

- **Version:** v1.14.0
- **Upstream:** https://github.com/google/googletest
- **License:** BSD-3-Clause
- **Used for:** the unit and integration test suite

Fetched only when `ORBIT_BUILD_TESTS=ON`. It is never part of the installed
package. Package builds (the vcpkg port and the Conan recipe) disable tests
precisely so this download is not required.

---

## Linked dependencies

Supplied by the system, vcpkg, or Conan. Orbit links against these; it does not
redistribute them. Each remains under its own license.

| Component | License | Required for |
|-----------|---------|--------------|
| [OpenSSL](https://www.openssl.org/) | Apache-2.0 | TLS, QUIC crypto, JWT signing — always |
| [zlib](https://zlib.net/) | zlib | Compression middleware, WebSocket permessage-deflate — always |
| [libcurl](https://curl.se/libcurl/) | curl (MIT-like) | Reverse proxy and outbound HTTP — always |
| [nghttp2](https://nghttp2.org/) | MIT | HTTP/2 — always |
| [ngtcp2](https://github.com/ngtcp2/ngtcp2) | MIT | QUIC transport — `ORBIT_ENABLE_HTTP3` |
| [nghttp3](https://github.com/ngtcp2/nghttp3) | MIT | HTTP/3 — `ORBIT_ENABLE_HTTP3` |
| [liburing](https://github.com/axboe/liburing) | MIT / LGPL-2.1 | `io_uring` event backend — Linux only |
| [libpq](https://www.postgresql.org/) | PostgreSQL License | PostgreSQL client — `ORBIT_ENABLE_POSTGRES` |
| [MariaDB Connector/C](https://mariadb.com/kb/en/mariadb-connector-c/) | LGPL-2.1 | MySQL/MariaDB client — `ORBIT_ENABLE_MARIADB` |
| [mongo-c-driver](https://github.com/mongodb/mongo-c-driver) | Apache-2.0 | MongoDB client — `ORBIT_ENABLE_MONGODB` |
| [hiredis](https://github.com/redis/hiredis) | BSD-3-Clause | Redis client — `ORBIT_ENABLE_REDIS` |
| [gRPC](https://grpc.io/) | Apache-2.0 | gRPC server wrapper — `ORBIT_ENABLE_GRPC` (off by default) |

### A note on copyleft

MariaDB Connector/C is LGPL-2.1, and liburing is dual MIT / LGPL-2.1. Linking
LGPL libraries statically carries obligations that dynamic linking does not. If
you distribute a statically linked binary and this matters for your situation,
either link those components dynamically or build with
`-DORBIT_ENABLE_MARIADB=OFF`. Orbit's own code is MIT and imposes no such
requirement.

This summary is provided for convenience and is not legal advice. Verify the
licenses of the exact versions your build resolves.

---

## Reporting an attribution problem

If a component is missing, misattributed, or the license shown here is wrong,
please open an issue — or a pull request correcting this file.
