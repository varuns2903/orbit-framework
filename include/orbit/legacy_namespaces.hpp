#pragma once

// Orbit's public API lives in namespace orbit (orbit::server, orbit::http,
// ...) since 2.0 (#39). The old top-level names stay available as aliases so
// code written for 1.x keeps compiling:
//
//     server::App app(cfg);   // same as orbit::server::App
//
// They put generic names such as server, http and config in the global
// namespace, which clashes with any code that uses those names itself.
// Define ORBIT_NO_LEGACY_NAMESPACES (before including any Orbit header, or
// on the compiler command line) to leave them out. They will be removed in
// Orbit 3.0.

namespace orbit {
namespace concurrency {}
namespace config {}
namespace database {}
namespace http {}
namespace middleware {}
namespace network {}
namespace openapi {}
namespace orm {}
namespace routing {}
namespace server {}
namespace utils {}
namespace websocket {}
} // namespace orbit

#ifndef ORBIT_NO_LEGACY_NAMESPACES
namespace concurrency = orbit::concurrency;
namespace config = orbit::config;
namespace database = orbit::database;
namespace http = orbit::http;
namespace middleware = orbit::middleware;
namespace network = orbit::network;
namespace openapi = orbit::openapi;
namespace orm = orbit::orm;
namespace routing = orbit::routing;
namespace server = orbit::server;
namespace utils = orbit::utils;
namespace websocket = orbit::websocket;
#endif
