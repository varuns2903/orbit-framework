# Routing in Orbit

Exact routes are found with one hash-map lookup; routes with `:params` or a wildcard are checked in registration order (a radix tree is planned, see [#179](https://github.com/varuns2903/orbit-framework/issues/179)). The syntax is inspired by Express.js.

## Basic Routing

```cpp
app.get("/hello", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    res->send(HttpResponse().send("Hello!"));
});

app.post("/submit", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    res->send(HttpResponse().send("Submitted!"));
});
```

## Dynamic Parameters

You can define URL parameters using the `:` prefix. They are extracted and made available in `req.params`.

```cpp
app.get("/users/:id", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    std::string user_id = req.params["id"];
    res->send(HttpResponse().send("User ID: " + user_id));
});
```

### Typed Parameters

`{name:type}` constrains a segment to a type, so a value that doesn't fit
means **this route doesn't match** — it falls through to the next route, and
on to 404 if nothing else matches — instead of reaching the handler as text
that then fails to parse:

```cpp
app.get("/tasks/{id:int}", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    // int64_t, not int: {id:int} accepts the type's full 64-bit range, so a
    // narrower T (plain int, say) could still overflow and come back nullopt
    // for a value the route itself already accepted.
    std::int64_t id = *req.param<std::int64_t>("id"); // always parses: the route already checked
    res->send(HttpResponse().send("Task " + std::to_string(id)));
});
```

- `int`: an optional leading `-`, then digits, parseable as a 64-bit integer
  (use `std::int64_t`, or wider, with `param<T>` to get the same guarantee —
  a narrower `T` can still see a value the route accepted overflow it).
- `uuid`: the canonical 8-4-4-4-12 hex form, hyphens included, hex digits
  matched case-insensitively.
- `str`, or no type (`{name}`): any non-empty segment, the same as `:name`.
- An unknown type (`{id:money}`) throws `std::invalid_argument` when the
  route is registered, not at request time.
- `req.param<T>(name)` parses anything in `req.params` — a typed segment or
  a plain `:name` — as `T` (`std::string`, `bool`, or an integral or
  floating-point type), returning `std::nullopt` if it is absent or does
  not parse. It does not read `req.query`. For a `{id:int}` segment and a
  `T` at least as wide as that type's range (`std::int64_t` for `int`), it
  never returns `std::nullopt` for a request the route matched; for a
  plain `:id` it replaces a hand-written `std::stoi` plus `try`/`catch`.

A typed and an untyped pattern for the same position can coexist; routes are
still tried in registration order, so put the more specific one first if
both could match the same request (see `app.validate_routes()` below for
catching an exact duplicate).

## Wildcards

A `*` as the **last** segment matches the rest of the path, zero or more
segments, and stores it in `req.params["*"]` (or under a name: `*path`):

```cpp
app.get("/files/*path", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    // GET /files/docs/a.txt -> req.params["path"] == "docs/a.txt"
    // GET /files            -> req.params["path"] == ""
    res->send(HttpResponse().send("file: " + req.params["path"]));
});
```

When several routes match, an exact route wins, then a `:param` route, then the
wildcard route with the most fixed segments (`/files/img/*` before `/files/*`).
A wildcard anywhere but last (`/a/*/b`) throws `std::invalid_argument` when the
route is added.

## Middleware for a Path Prefix

`app.use(prefix, middleware)` (or `router.use` inside a group) runs the
middleware only for the prefix and the paths below it: `/admin` and
`/admin/...`, not `/administrator`. It runs before route matching, so it can
answer paths that have no route of their own (a reverse proxy, a static
directory):

```cpp
app.use("/admin", require_admin);
app.use("/api", orbit::middleware::proxy(api_upstream));
```

## Not Found

`app.not_found(handler)` answers requests that match no route, in place of the
plain `404 Not Found`. A path that exists under another method still gets
`405 Method Not Allowed` with an `Allow` header.

```cpp
app.not_found([](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    HttpResponse out;
    out.status(HttpStatus::NotFound).json(nlohmann::json{{"error", "not found"}, {"path", req.uri}});
    res->send(std::move(out));
});
```

## Catching Duplicate Routes

The same method and exact path pattern registered twice is almost always a
mistake, and it fails silently: for a static path, the later registration
quietly replaces the earlier one; for a dynamic one (`:id`, `{id:int}`,
`*`), the first one keeps matching and the second is never reached. Neither
warns.

`app.listen()` calls `app.validate_routes()` before binding any socket and
refuses to start (`std::invalid_argument`, listing every duplicate) if it
finds one. Call `validate_routes()` yourself to check without starting a
server, e.g. in a test:

```cpp
EXPECT_TRUE(app.validate_routes().empty());
```

It only catches an *exact* duplicate (identical method and pattern text);
two patterns that could overlap at request time (`:id` and `{id:int}` for
the same position) are not flagged, since the router's "first match in
registration order" rule already makes that well-defined.

## Route Grouping

To group API endpoints under a common prefix, use `app.group()`. This is useful for versioning your APIs.

```cpp
app.group("/api/v1", [](orbit::routing::Router& api) {
    api.use(require_api_key);   // runs for every route in the group

    api.get("/status", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().send("v1 Status OK"));
    });

    api.post("/login", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        // Login logic
    });
});
```

A group can have its own error handler. `api.on_error(...)` covers the
group's routes (and its middleware), including routes registered before the
call, and nested groups that don't set their own. Every other exception goes
to `app.on_error`, as do those from app-level middleware, which runs before
a route is matched.

```cpp
app.on_error(render_html_error);          // everything else
app.group("/api", [](orbit::routing::Router& api) {
    api.on_error(render_json_error);      // only /api/...
    api.get("/items/:id", get_item);
});
```

## Streaming Responses (Chunked)

Orbit natively supports chunked transfer encoding for streaming large amounts of data without buffering it entirely in memory:

```cpp
app.get("/stream", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    HttpResponse response;
    response.headers["Transfer-Encoding"] = "chunked";
    res->send_headers(response);
    
    res->write_chunk("This is chunk 1\n");
    res->write_chunk("This is chunk 2\n");
    res->end(); // Important: Sends the 0\r\n\r\n terminator
});
```

### When the Client Leaves

A writer kept after the handler returns (an SSE subscriber, a long chunked
stream) can find out that its client is gone: `is_open()` turns false, and
callbacks registered with `on_close()` run once, when the connection
(HTTP/1.1) or stream (HTTP/2, HTTP/3) closes, because the client left, a
timeout fired or the server is shutting down. Writes after that are dropped.

```cpp
std::mutex mu;
std::vector<std::shared_ptr<ResponseWriter>> subscribers;

app.get("/events", [&](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
    HttpResponse res;
    res.headers["Content-Type"] = "text/event-stream";
    w->send_headers(res);

    std::weak_ptr<ResponseWriter> weak = w;   // the list owns it, not the callback
    w->on_close([&, weak] {
        std::lock_guard<std::mutex> lock(mu);
        if (auto gone = weak.lock()) std::erase(subscribers, gone);
    });
    std::lock_guard<std::mutex> lock(mu);
    subscribers.push_back(w);
});
```

The callback runs on a server thread: keep it short, and do not block in it.
A callback registered after the close runs right away.

## Request Bodies

### Forms

`req.form_fields()` decodes an `application/x-www-form-urlencoded` body (an HTML form without files). It is empty for other content types or a malformed body:

```cpp
app.post("/signup", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    auto fields = req.form_fields();
    HttpResponse response;
    response.set_body("Welcome, " + fields["name"]);
    res->send(std::move(response));
});
```

`req.form()` parses a buffered `multipart/form-data` body in memory, which suits small forms.

### File uploads streamed to disk

For uploads, register a **stream route**, so the body is not buffered in memory, and pass it to `orbit::http::receive_multipart`. File parts are written as they arrive to private, randomly named files (`MultipartLimits::upload_dir`, by default a 0700 per-process directory under the system temp directory), within the limits you set:

```cpp
#include <orbit/http/MultipartUpload.hpp>

app.group("/api", [](orbit::routing::Router& api) {
    api.add_stream_route(HttpMethod::POST, "/upload", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        orbit::http::MultipartLimits limits;
        limits.max_file_size = 50 * 1024 * 1024;
        orbit::http::receive_multipart(req, res, [res](orbit::http::MultipartUpload& upload) {
            HttpResponse response;
            if (!upload.ok()) {
                response.status(HttpStatus::BadRequest).send(upload.error);
            } else {
                for (const auto& file : upload.files) {
                    // file.path is yours: move it into place. file.filename is
                    // chosen by the client; never use it as a path.
                }
                response.send("saved " + std::to_string(upload.files.size()) + " file(s)");
            }
            res->send(std::move(response));
        }, limits);
    });
});
```

The upload fails, and every file it saved is deleted, if a limit is exceeded or the body is malformed or truncated. Files from a successful upload belong to the application; `upload.discard()` deletes them.

### `Expect: 100-continue`

HTTP/1.1 clients that send `Expect: 100-continue` (curl does for larger bodies) receive `100 Continue` once the headers have been accepted, then send the body. A body over `max_body_size` is refused with 413 before it is sent.
