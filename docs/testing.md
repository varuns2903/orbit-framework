# Testing Your Application

`orbit::testing::Client` calls an application's routes in-process: no
`listen()`, no ports, no sockets. Tests run in milliseconds and never collide
on a port.

```cpp
#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/testing/Client.hpp>

TEST(Tasks, CreateAndFetch) {
    orbit::server::App app(orbit::config::ServerConfig{});
    register_routes(app);                       // the same function main() uses

    orbit::testing::Client client(app);
    auto created = client.post("/api/tasks").json({{"title", "write tests"}}).send();
    ASSERT_EQ(created.status, 201);

    auto id = created.json()["id"].get<int>();
    auto fetched = client.get("/api/tasks/" + std::to_string(id)).send();
    EXPECT_EQ(fetched.json()["title"], "write tests");
}
```

## What runs

The request goes through the same router as on the network: global, prefix
and group middleware, routing (including 404 and 405 with `Allow`),
`on_error` handlers, `not_found`, sessions and anything a middleware adds to
the response. What the network layer itself adds (`Date`, `Content-Length`
framing, HTTP/2) is not applied.

## Requests

```cpp
client.get("/search?q=orbit")
      .header("Authorization", "Bearer " + token)
      .cookie("theme", "dark")                 // this request only
      .client_ip("203.0.113.7")                // what req.client_ip says (default 127.0.0.1)
      .send();

client.post("/upload").body(csv, "text/csv").send();
client.put("/items/1").json(item).send();
client.del("/items/1").send();
```

`send(timeout)` waits until the response is complete, five seconds by
default. An application error is a response, never an exception: a 500, or
whatever `on_error` answers.

## Responses

`Response` has `status`, `headers` (look them up with `header("name")`, case
insensitive), `body`, `json()`, `cookies` (the `Set-Cookie` entries) and, for
streamed responses, `chunks`.

## Cookies and sessions

The client keeps a cookie jar (`client.cookies()`): cookies the application
sets are sent with later requests, so a login followed by an authenticated
request works as in a browser. Use a second `Client` for a second user.

## Coroutine handlers

Handlers that `co_await` work as they do in the server. The client runs a
small thread pool and an event loop of its own, which `writer->thread_pool()`
and `writer->proactor()` return, and `send()` waits for the response however
it arrives.

## Streams and SSE

Chunks from `write_chunk()` and events from `send_sse_event()` are collected
in `chunks`, already in wire format (`"event: update\nid: 2\ndata: two\n\n"`).
A stream that never ends (a live SSE feed) returns when `send()`'s timeout
passes, with `complete == false` and what was sent so far:

```cpp
auto feed = client.get("/events").send(std::chrono::milliseconds(200));
EXPECT_FALSE(feed.complete);
EXPECT_EQ(feed.chunks.front(), "data: hello\n\n");
```

When `send()` returns, the "client" leaves: a writer the application kept
reports `is_open() == false` and runs its `on_close()` callbacks, as if the
browser had closed the tab.

## Not supported (yet)

WebSocket upgrades and raw streams (`upgrade_to_raw_stream`). Test those
against a running server, as `tests/integration` does.
