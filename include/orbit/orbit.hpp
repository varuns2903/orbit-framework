#pragma once
/**
 * @file
 * @brief Everything a typical Orbit application needs, in one include.
 *
 * ```cpp
 * #include <orbit/orbit.hpp>
 *
 * int main() {
 *     orbit::App app(orbit::config::ServerConfig{});
 *     app.get("/hello", [](orbit::Request&, orbit::Writer w) {
 *         w->send(orbit::Response().json({{"message", "hi"}}));
 *     });
 *     app.listen();
 * }
 * ```
 *
 * Leaves out the database clients and ORM (`<orbit/orm/*.hpp>`,
 * `<orbit/database/*.hpp>`): they pull in vcpkg dependencies (libpq,
 * libmysqlclient, mongoc, ...) that not every application uses, so they
 * stay separate includes. Everything else — the server, routing, the
 * built-in middleware, WebSockets, the test client, outbound HTTP — is
 * here.
 */
#include <orbit/legacy_namespaces.hpp>

// Server and routing
#include <orbit/server/App.hpp>
#include <orbit/server/Timer.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/routing/HandlerWrapper.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/config/ConfigFile.hpp>

// HTTP: requests, responses, bodies, the outbound client
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/http/RequestContext.hpp>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>
#include <orbit/http/MultipartUpload.hpp>
#include <orbit/http/Client.hpp>

// WebSockets
#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/websocket/EventRouter.hpp>

// Coroutines
#include <orbit/concurrency/Task.hpp>
#include <orbit/concurrency/Awaitable.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

// OpenAPI / Swagger
#include <orbit/openapi/OpenApi.hpp>

// Built-in middleware
#include <orbit/middleware/Compress.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/Csrf.hpp>
#include <orbit/middleware/DistributedRateLimiter.hpp>
#include <orbit/middleware/GraphQL.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/Metrics.hpp>
#include <orbit/middleware/OAuth2.hpp>
#include <orbit/middleware/Observability.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/SecurityHeaders.hpp>
#include <orbit/middleware/SessionManager.hpp>
#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/middleware/TrustedProxies.hpp>
#include <orbit/middleware/Validation.hpp>

// Testing
#include <orbit/testing/Client.hpp>

// Logging
#include <orbit/utils/Logger.hpp>

#include <memory>

namespace orbit {

/// @name Short aliases for the names an application writes constantly.
/// Everything here is also reachable at its full, namespaced name
/// (`orbit::server::App`, `orbit::http::HttpResponse`, ...); these are
/// nothing more than `using` aliases to it, so either spelling works
/// interchangeably, including in code that mixes the two.
/// @{
using App = server::App;
using Request = http::HttpRequest;
using Response = http::HttpResponse;
using Status = http::HttpStatus;
using Method = http::HttpMethod;
/// What a handler or middleware receives to answer a request:
/// `std::shared_ptr<http::ResponseWriter>`.
using Writer = std::shared_ptr<http::ResponseWriter>;
using RouteHandler = routing::RouteHandler;
using Middleware = routing::Middleware;
/// nlohmann::json, Orbit's JSON type, already used throughout
/// `Request`/`Response` (`req.json()`, `res.json(...)`).
using json = nlohmann::json;
/// @}

} // namespace orbit
