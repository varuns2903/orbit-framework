# HTTP Client

`orbit::http::Client` makes outbound HTTP requests (to other services, OAuth2 providers, webhooks) without blocking the server's worker threads. One background thread drives every transfer through libcurl's multi interface, so many requests run concurrently and connections to the same host are kept alive and reused.

```cpp
#include <orbit/http/Client.hpp>
```

## Coroutines

```cpp
orbit::concurrency::Task notify(std::shared_ptr<orbit::http::ResponseWriter> writer) {
    orbit::http::ClientRequest req;
    req.method = "POST";
    req.url = "https://hooks.example.com/orders";
    req.headers = {{"Content-Type", "application/json"}};
    req.body = R"({"order": 42})";

    orbit::http::ClientResponse res = co_await orbit::http::Client::shared().send_async(req);

    orbit::http::HttpResponse out;
    if (!res.ok()) {
        out.status(orbit::http::HttpStatus::BadGateway).send("upstream failed: " + res.error);
    } else {
        out.send("upstream said " + std::to_string(res.status));
    }
    writer->send(std::move(out));
}
```

## Callbacks and blocking calls

```cpp
orbit::http::Client::shared().send(req, [](orbit::http::ClientResponse res) {
    // runs on the client's thread
});

// From code that has to block (not from inside a callback):
orbit::http::ClientResponse res = orbit::http::Client::shared().send_sync(req);
```

`res.ok()` means a response arrived; check `res.status` for the HTTP status (a 404 is a successful *request*). Failures (DNS, connection refused, TLS, timeout, size limit) set `res.error` and leave `status` at 0. `res.header("Content-Type")` looks a header up case-insensitively; `res.effective_url` is the URL after redirects.

## Options per request

| Field | Default | |
|---|---|---|
| `timeout` | 30 s | Whole request, redirects included |
| `connect_timeout` | 10 s | |
| `follow_redirects` / `max_redirects` | on / 5 | Only to `http`/`https` URLs |
| `verify_tls` | on | Certificate chain and host name |
| `ca_file` | system store | PEM bundle, e.g. for a private CA |
| `max_response_size` | 10 MB | Larger responses fail instead of filling memory |

Only `http://` and `https://` URLs are fetched, including on redirects, so a redirect cannot make the server read `file://` or other schemes. Compressed responses are decoded automatically.

## Threads

Callbacks, and coroutines resumed by `send_async()`, run on the client's thread. Keep that work short: answer the request, or hand anything slow to the thread pool. `send_sync()` called on that thread fails immediately rather than waiting forever.

`orbit::http::Client::shared()` is a process-wide instance; create your own `orbit::http::Client` for a separate connection pool or user agent (`ClientOptions::user_agent`). Destroying a client completes its pending requests with the error `client shut down`.

The OAuth2 middleware and JWKS key fetching (`jwt_auth`) use the shared client.
