# Routing in Orbit

Orbit uses a fast radix-trie and hash-map based router that executes in O(1) for static routes and O(log N) for dynamic routes. The syntax is heavily inspired by Express.js.

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

## Route Grouping

To group API endpoints under a common prefix, use `app.group()`. This is useful for versioning your APIs.

```cpp
auto api_v1 = app.group("/api/v1");

api_v1->get("/status", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    res->send(HttpResponse().send("v1 Status OK"));
});

api_v1->post("/login", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
    // Login logic
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
