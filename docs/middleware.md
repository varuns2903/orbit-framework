# Middleware in Orbit

Middleware functions have access to the request object, the response writer, and can intercept or modify the flow of the application. 
They return a `bool`: `true` to continue to the next handler, `false` to halt execution (e.g., if the middleware already sent a response).

## Defining Middleware

```cpp
auto logger = [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) -> bool {
    std::cout << req.method_string() << " " << req.uri << "\n";
    return true; // Continue to next handler
};
```

## Global Middleware

Use `app.use()` to apply middleware to every incoming request.

```cpp
app.use(logger);
app.use(middleware::cors());
```

## Sessions

`middleware::session()` gives every client a session and exposes it as
`req.session` (and its identifier as `req.session_id`). Data is stored on the
server; the client only holds an opaque `HttpOnly` cookie.

```cpp
#include <orbit/middleware/SessionManager.hpp>

middleware::SessionOptions opts;
opts.secure = true;          // send the cookie only over HTTPS
opts.same_site = "Lax";
opts.ttl_seconds = 3600;     // idle lifetime, refreshed on every request

// In-process store (single server):
app.use(middleware::session(std::make_shared<middleware::MemorySessionStore>(), opts));
// Shared across instances (needs ORBIT_ENABLE_REDIS):
app.use(middleware::session("127.0.0.1", 6379, opts));

app.post("/login", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> res) {
    // ... check credentials ...
    req.session->regenerate();              // new id after login (prevents session fixation)
    req.session->set("user_id", "42");
    res->send(http::HttpResponse().send("welcome"));
});

app.get("/me", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> res) {
    auto user = req.session->get("user_id"); // std::optional<std::string>
    res->send(http::HttpResponse().send(user.value_or("anonymous")));
});

app.post("/logout", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> res) {
    req.session->destroy();                 // deletes the data and expires the cookie
    res->send(http::HttpResponse().send("bye"));
});
```

- Changes are saved when the response is sent; unchanged sessions only have
  their lifetime refreshed.
- Identifiers are 256-bit values from the system CSPRNG. A cookie is accepted
  only if the store holds that session, so clients cannot choose their own id.
- `save_uninitialized = false` creates no session (and sends no cookie) until
  something is written, so anonymous traffic and bots do not fill the store.
- Implement `middleware::SessionStore` (`load`, `save`, `touch`, `destroy`) to
  keep sessions elsewhere, e.g. in a database.

## CORS

`middleware::cors()` with no arguments allows every origin (`Access-Control-Allow-Origin: *`)
without credentials. To restrict origins, list them:

```cpp
middleware::CorsOptions cors;
cors.allowed_origins = {"https://app.example.com", "https://admin.example.com"};
cors.allow_credentials = true;
app.use(middleware::cors(cors));
```

With an allow-list, only a listed `Origin` is echoed back in
`Access-Control-Allow-Origin`, `Vary: Origin` is always sent so shared caches
keep per-origin copies, and unlisted origins get no CORS headers (the browser
then blocks the response). Preflights (`OPTIONS` with
`Access-Control-Request-Method`) are answered with `204`; other `OPTIONS`
requests reach your routes. `allow_credentials` is ignored when
`allowed_origins` contains `"*"`, because allowing credentials from every origin
would let any website act as the signed-in user.

## JWT Authentication

`middleware::jwt_auth()` verifies an `Authorization: Bearer <token>` header and
stores the token's claims in `req.user`. Only HS256 tokens are accepted: the
algorithm comes from the server configuration, never from the token header.

```cpp
#include <orbit/middleware/JwtAuth.hpp>

middleware::JwtOptions jwt;
jwt.secret = std::getenv("JWT_SECRET");   // at least 32 random bytes
jwt.issuer = "https://auth.example.com";  // optional "iss" check
jwt.audience = "orders-api";              // optional "aud" check
jwt.require_exp = true;                   // reject tokens that never expire
jwt.leeway = std::chrono::seconds(30);    // tolerated clock skew
app.use(middleware::jwt_auth(jwt));
```

`exp` and `nbf` are enforced whenever they are present. Failures return
`401` with `WWW-Authenticate: Bearer`. An empty secret throws
`std::invalid_argument`; secrets shorter than 32 bytes log a warning.

## Route-Specific Middleware

You can inject middleware into specific routes using an initializer list `vector<Middleware>`:

```cpp
auto auth_guard = [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) -> bool {
    if (req.headers["Authorization"].empty()) {
        res->send(HttpResponse().status(HttpStatus::Unauthorized).send("Missing Auth"));
        return false; // Stop execution
    }
    return true;
};

app.get("/dashboard", {auth_guard}, [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    res->send(HttpResponse().send("Welcome to the secret dashboard!"));
});
```

## Built-in Middleware

Orbit comes with Several high-performance built-in middlewares:

### JSON Schema Validation
Automatically validates request bodies and returns `422 Unprocessable Entity` if the JSON is malformed.
```cpp
#include "middleware/Validation.hpp"

std::vector<SchemaField> user_schema = {
    {"username", JsonType::STRING, true},
    {"age", JsonType::NUMBER, true}
};

app.post("/users", {middleware::validate_json(user_schema)}, [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    // req.json() is now guaranteed to be valid!
});
```

### Static Files
Serves files from a directory for `GET` and `HEAD`; anything not found falls through to
the next middleware or route.
```cpp
#include <orbit/middleware/StaticFiles.hpp>
app.use(middleware::static_files("public"));

middleware::StaticFilesOptions opts;
opts.max_age = std::chrono::hours(24); // Cache-Control: public, max-age=86400
opts.index = "index.html";            // served for directory requests; "" disables
opts.serve_dotfiles = false;          // .env, .git/ ... are hidden (the default)
app.use(middleware::static_files("public", opts));
```
- Paths that resolve outside the directory, including through symlinks, get `403`.
- Responses carry `ETag`, `Last-Modified`, `Cache-Control` and `Accept-Ranges: bytes`.
  `If-None-Match` and `If-Modified-Since` give `304`.
- A single byte range (`Range: bytes=0-1023`, `bytes=-500`, ...) gives `206` with
  `Content-Range`, or `416` if it starts past the end. `If-Range` is honoured. Multi-range
  requests get the whole file.
- `middleware::mime_type_for_extension(".woff2")` exposes the built-in type table.

### Rate Limiting
Global in-memory or Redis-backed distributed rate limiting. Rejected requests get
`429 Too Many Requests` with a `Retry-After` header (seconds).

The in-memory limiter is a token bucket: each client may burst up to `max_requests`
and regains capacity continuously at `max_requests / window`. Idle clients are evicted,
and at most `max_tracked_clients` are tracked, so memory stays bounded under IP spraying.
```cpp
#include <orbit/middleware/RateLimiter.hpp>
app.use(middleware::rate_limit(100, std::chrono::seconds(10))); // 100 reqs per 10s

// Behind a proxy, or per API key instead of per IP:
middleware::RateLimitOptions opts;
opts.max_requests = 1000;
opts.window = std::chrono::seconds(60);
opts.key = [](const HttpRequest& req) {
    auto it = req.headers.find("X-Api-Key");
    return it != req.headers.end() ? std::string(it->second) : req.client_ip;
};
opts.max_tracked_clients = 50000;
app.use(middleware::rate_limit(opts));
```

The Redis-backed limiter shares a fixed window across all instances. The counter and its
expiry are set atomically. If Redis is unreachable, requests are rejected with `503` unless
`allow_when_unavailable` is `true`.
```cpp
#include <orbit/middleware/DistributedRateLimiter.hpp>
app.use(middleware::distributed_rate_limit("127.0.0.1", 6379, 100, std::chrono::seconds(60)));
```

### Global Error Handling
Catch all unhandled exceptions thrown inside route handlers:
```cpp
app.on_error([](const std::exception& e, HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
    writer->send(HttpResponse().status(HttpStatus::InternalServerError).send("Something went wrong!"));
});
```
