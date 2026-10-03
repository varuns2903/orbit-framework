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
