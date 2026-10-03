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
app.use(middleware::Cors::allow_all());
```

## Sessions

`middleware::session()` gives every client a session identifier stored in
Redis and exposes it as `req.session_id`:

```cpp
#include <orbit/middleware/SessionManager.hpp>

middleware::SessionOptions opts;
opts.secure = true;          // send the cookie only over HTTPS
opts.same_site = "Lax";
opts.ttl_seconds = 3600;     // idle lifetime, refreshed on every request
app.use(middleware::session("127.0.0.1", 6379, opts));
```

Identifiers are 256-bit values from the system CSPRNG (`utils::secure_random_hex`).
A `session_id` cookie is accepted only if Redis holds that identifier, so
clients cannot pick their own session (session fixation). Requires
`ORBIT_ENABLE_REDIS=ON`.

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

### Rate Limiting
Global in-memory or Redis-backed distributed rate limiting.
```cpp
#include "middleware/RateLimiter.hpp"
app.use(middleware::rate_limit(100, std::chrono::seconds(10))); // 100 reqs per 10s
```

### Global Error Handling
Catch all unhandled exceptions thrown inside route handlers:
```cpp
app.on_error([](const std::exception& e, HttpRequest& req, std::shared_ptr<ResponseWriter> writer) {
    writer->send(HttpResponse().status(HttpStatus::InternalServerError).send("Something went wrong!"));
});
```
