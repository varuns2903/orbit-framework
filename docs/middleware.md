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
app.use(orbit::middleware::cors());
```

## Sessions

`orbit::middleware::session()` gives every client a session and exposes it as
`req.session` (and its identifier as `req.session_id`). Data is stored on the
server; the client only holds an opaque `HttpOnly` cookie.

```cpp
#include <orbit/middleware/SessionManager.hpp>

orbit::middleware::SessionOptions opts;
opts.secure = true;          // send the cookie only over HTTPS
opts.same_site = "Lax";
opts.ttl_seconds = 3600;     // idle lifetime, refreshed on every request

// In-process store (single server):
app.use(orbit::middleware::session(std::make_shared<orbit::middleware::MemorySessionStore>(), opts));
// Shared across instances (needs ORBIT_ENABLE_REDIS):
app.use(orbit::middleware::session("127.0.0.1", 6379, opts));

app.post("/login", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> res) {
    // ... check credentials ...
    req.session->regenerate();              // new id after login (prevents session fixation)
    req.session->set("user_id", "42");
    res->send(orbit::http::HttpResponse().send("welcome"));
});

app.get("/me", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> res) {
    auto user = req.session->get("user_id"); // std::optional<std::string>
    res->send(orbit::http::HttpResponse().send(user.value_or("anonymous")));
});

app.post("/logout", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> res) {
    req.session->destroy();                 // deletes the data and expires the cookie
    res->send(orbit::http::HttpResponse().send("bye"));
});
```

- Changes are saved when the response is sent; unchanged sessions only have
  their lifetime refreshed.
- Identifiers are 256-bit values from the system CSPRNG. A cookie is accepted
  only if the store holds that session, so clients cannot choose their own id.
- `save_uninitialized = false` creates no session (and sends no cookie) until
  something is written, so anonymous traffic and bots do not fill the store.
- Implement `orbit::middleware::SessionStore` (`load`, `save`, `touch`, `destroy`) to
  keep sessions elsewhere, e.g. in a database.

## CORS

`orbit::middleware::cors()` with no arguments allows every origin (`Access-Control-Allow-Origin: *`)
without credentials. To restrict origins, list them:

```cpp
orbit::middleware::CorsOptions cors;
cors.allowed_origins = {"https://app.example.com", "https://admin.example.com"};
cors.allow_credentials = true;
app.use(orbit::middleware::cors(cors));
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

`orbit::middleware::jwt_auth()` verifies an `Authorization: Bearer <token>` header and
stores the token's claims in `req.user`. HS256, RS256 and ES256 are supported.
Each configured key has exactly one algorithm, and a token must use the
algorithm of the key it is checked against, so it cannot pick a weaker one
(e.g. `none`, or HS256 with the public key as the secret).

```cpp
#include <orbit/middleware/JwtAuth.hpp>

orbit::middleware::JwtOptions jwt;
jwt.secret = std::getenv("JWT_SECRET");   // at least 32 random bytes
jwt.issuer = "https://auth.example.com";  // optional "iss" check
jwt.audience = "orders-api";              // optional "aud" check
jwt.require_exp = true;                   // reject tokens that never expire
jwt.leeway = std::chrono::seconds(30);    // tolerated clock skew
app.use(orbit::middleware::jwt_auth(jwt));
```

`exp` and `nbf` are enforced whenever they are present. Failures return
`401` with `WWW-Authenticate: Bearer`. Configuring no key throws
`std::invalid_argument`; secrets shorter than 32 bytes log a warning.

**Public keys (RS256 / ES256).** Tokens from an identity provider (Auth0,
Keycloak, Cognito, Azure AD, ...) are signed with a private key; verify them
with the public key, or let Orbit fetch the provider's key set:

```cpp
orbit::middleware::JwtOptions jwt;
// One key: an RSA key (>= 2048 bits) verifies RS256, an EC P-256 key ES256.
jwt.public_key_pem = read_file("issuer-public.pem");
// Or the provider's JSON Web Key Set; the token's "kid" picks the key.
jwt.jwks_url = "https://auth.example.com/.well-known/jwks.json";
jwt.jwks_refresh = std::chrono::hours(1);       // refetch at least this often
jwt.issuer = "https://auth.example.com/";
jwt.audience = "orders-api";
app.use(orbit::middleware::jwt_auth(jwt));
```

The key set is fetched on first use and cached. A token whose `kid` is unknown
triggers a refetch (so key rotation works without a restart), but never more
often than `jwks_min_refetch` (default 30 s), so forged `kid`s cannot flood the
provider. If a fetch fails, the previous keys stay in use.

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
#include <orbit/middleware/Validation.hpp>

std::vector<SchemaField> user_schema = {
    {"username", JsonType::STRING, true},
    {"age", JsonType::NUMBER, true}
};

app.post("/users", {orbit::middleware::validate_json(user_schema)}, [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    // req.json() is now guaranteed to be valid!
});
```

### Static Files
Serves files from a directory for `GET` and `HEAD`; anything not found falls through to
the next middleware or route.
```cpp
#include <orbit/middleware/StaticFiles.hpp>
app.use(orbit::middleware::static_files("public"));

orbit::middleware::StaticFilesOptions opts;
opts.max_age = std::chrono::hours(24); // Cache-Control: public, max-age=86400
opts.index = "index.html";            // served for directory requests; "" disables
opts.serve_dotfiles = false;          // .env, .git/ ... are hidden (the default)
app.use(orbit::middleware::static_files("public", opts));
```
- Paths that resolve outside the directory, including through symlinks, get `403`.
- Responses carry `ETag`, `Last-Modified`, `Cache-Control` and `Accept-Ranges: bytes`.
  `If-None-Match` and `If-Modified-Since` give `304`.
- A single byte range (`Range: bytes=0-1023`, `bytes=-500`, ...) gives `206` with
  `Content-Range`, or `416` if it starts past the end. `If-Range` is honoured. Multi-range
  requests get the whole file.
- `orbit::middleware::mime_type_for_extension(".woff2")` exposes the built-in type table.

### Compression

`orbit::middleware::compress()` compresses response bodies with brotli (`br`), zstd or gzip, whichever the client's `Accept-Encoding` allows with the highest q-value; ties go to the server's preference (brotli, zstd, gzip). Every compressible response gets `Vary: Accept-Encoding`. Bodies that are small, already encoded, already-compressed media, streamed, or marked `Cache-Control: no-transform` are left alone, and a strong `ETag` on a compressed body becomes weak.

```cpp
#include <orbit/middleware/Compress.hpp>

app.use(orbit::middleware::compress());

// Or tune it:
orbit::middleware::CompressOptions opts;
opts.preference = {orbit::middleware::ContentCoding::Zstd, orbit::middleware::ContentCoding::Gzip};
opts.brotli_quality = 4; // 0-11
opts.zstd_level = 3;     // 1-19
opts.min_size = 512;
app.use(orbit::middleware::compress(opts));
```

Brotli and zstd come from the `ORBIT_ENABLE_BROTLI` and `ORBIT_ENABLE_ZSTD` build options (both on by default; with vcpkg, the `brotli` and `zstd` manifest features). A coding left out of the build is simply never chosen.

### Security Headers
Adds common hardening headers to every response. They are defaults: a handler
that sets the same header keeps its own value.
```cpp
#include <orbit/middleware/SecurityHeaders.hpp>
app.use(orbit::middleware::security_headers());
// Strict-Transport-Security: max-age=31536000; includeSubDomains
// X-Content-Type-Options: nosniff
// X-Frame-Options: DENY
// Referrer-Policy: strict-origin-when-cross-origin
// Cross-Origin-Opener-Policy: same-origin

orbit::middleware::SecurityHeadersOptions opts;
opts.content_security_policy = "default-src 'self'; frame-ancestors 'none'";
opts.frame_options = "";   // empty string: leave the header out
app.use(orbit::middleware::security_headers(opts));
```

### Trusted Proxies (real client IP)
Behind a load balancer or reverse proxy, the socket peer is the proxy. List your
proxies and `req.client_ip` becomes the real client, taken from
`X-Forwarded-For` (or RFC 7239 `Forwarded`). Requests that do **not** come from
a listed proxy keep their socket address, so clients cannot spoof their IP. The
socket address is always available as `req.peer_ip`.
```cpp
#include <orbit/middleware/TrustedProxies.hpp>
orbit::middleware::TrustedProxyOptions proxies;
proxies.proxies = {"10.0.0.0/8", "127.0.0.1", "::1"};
app.use(orbit::middleware::trusted_proxies(proxies));   // register before rate limiting
app.use(orbit::middleware::rate_limit(100, std::chrono::seconds(10)));
```
The header is read right to left, skipping listed proxies, so an address a
client prepends to `X-Forwarded-For` is never used.

### Rate Limiting
Global in-memory or Redis-backed distributed rate limiting. Rejected requests get
`429 Too Many Requests` with a `Retry-After` header (seconds).

The in-memory limiter is a token bucket: each client may burst up to `max_requests`
and regains capacity continuously at `max_requests / window`. Idle clients are evicted,
and at most `max_tracked_clients` are tracked, so memory stays bounded under IP spraying.
```cpp
#include <orbit/middleware/RateLimiter.hpp>
app.use(orbit::middleware::rate_limit(100, std::chrono::seconds(10))); // 100 reqs per 10s

// Behind a proxy, or per API key instead of per IP:
orbit::middleware::RateLimitOptions opts;
opts.max_requests = 1000;
opts.window = std::chrono::seconds(60);
opts.key = [](const HttpRequest& req) {
    auto it = req.headers.find("X-Api-Key");
    return it != req.headers.end() ? std::string(it->second) : req.client_ip;
};
opts.max_tracked_clients = 50000;
app.use(orbit::middleware::rate_limit(opts));
```

The Redis-backed limiter shares a fixed window across all instances. The counter and its
expiry are set atomically. If Redis is unreachable, requests are rejected with `503` unless
`allow_when_unavailable` is `true`.
```cpp
#include <orbit/middleware/DistributedRateLimiter.hpp>
app.use(orbit::middleware::distributed_rate_limit("127.0.0.1", 6379, 100, std::chrono::seconds(60)));
```

### Global Error Handling
Catch all unhandled exceptions thrown inside route handlers (a group can
override this for its own routes; see [Route Grouping](routing.md#route-grouping)):
```cpp
app.on_error([](const std::exception& e, HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
    writer->send(HttpResponse().status(HttpStatus::InternalServerError).send("Something went wrong!"));
});
```
