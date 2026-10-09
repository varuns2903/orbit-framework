# WebSockets

Orbit has built-in support for RFC 6455 WebSockets. Because the core engine is asynchronous, a single server instance can hold millions of concurrent WebSocket connections efficiently without exhausting OS threads.

## Defining a WebSocket Route

Instead of `app.get()`, use `app.ws()` and pass a handler that accepts a `WebSocketConnection&`.

```cpp
app.ws("/chat", [](orbit::http::websocket::WebSocketConnection& ws) {
    
    // Register message callback
    ws.on_message([&ws](const std::string& msg) {
        std::cout << "Received: " << msg << std::endl;
        
        // Echo it back
        ws.send("Echo: " + msg);
    });

    // Register close callback
    ws.on_close([]() {
        std::cout << "Client disconnected" << std::endl;
    });
    
});
```

## Message Size Limit

Each connection accepts messages of up to 16 MiB by default, measured after
permessage-deflate decompression. A frame whose header announces a larger
payload is refused before the payload is buffered, and the connection is closed
with status `1009` (Message Too Big). Malformed frames close it with `1002`.

Lower the limit inside the route handler, before any message arrives:

```cpp
app.ws("/chat", [](orbit::http::websocket::WebSocketConnection& ws) {
    ws.set_max_message_size(64 * 1024); // 64 KiB
    ws.on_message([&ws](const std::string& msg) { ws.send(msg); });
});
```

## Text and Binary Messages

`send()` sends a text frame and `send_binary()` a binary one. Incoming binary
messages go to `on_binary_message()` when it is set; otherwise `on_message()`
receives both kinds, as it always has.

```cpp
app.ws("/echo", [](orbit::http::websocket::WebSocketConnection& ws) {
    ws.on_message([&ws](const std::string& text) { ws.send(text); });
    ws.on_binary_message([&ws](const std::string& data) { ws.send_binary(data); });
});
```

`send()`, `send_binary()` and `close()` are safe to call from any thread, e.g. when
broadcasting from a worker.

## Protocol Conformance

Connections follow RFC 6455 and RFC 7692 (permessage-deflate):

- Fragmented messages are reassembled, and control frames may arrive between the fragments.
- Pongs echo the ping payload.
- Violations close the connection with the proper status code:
  - `1002` for an unmasked client frame, an RSV bit set without a negotiated extension, a reserved opcode, a bad fragment sequence, or an invalid close code.
  - `1007` for text, or a close reason, that is not valid UTF-8.
- A close frame is answered with the client's status code.
- Each compressed message uses a fresh deflate context, matching the negotiated `no_context_takeover`.

CI runs the [Autobahn TestSuite](https://github.com/crossbario/autobahn-testsuite)
against `examples/websocket_echo_server.cpp`; see `.github/workflows/autobahn.yml`.

## Securing WebSocket Routes

Global middleware (`app.use`), group middleware and route middleware run before
the handshake is accepted, exactly as for HTTP routes. A middleware that stops
the request (for example `jwt_auth` returning 401) aborts the upgrade.

Browsers do not apply CORS to WebSocket handshakes, so check `Origin` to prevent
cross-site WebSocket hijacking:

```cpp
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/JwtAuth.hpp>

app.ws("/chat",
       {orbit::middleware::require_origin({"https://app.example.com"}),
        orbit::middleware::jwt_auth(secret)},
       [](orbit::http::websocket::WebSocketConnection& ws) { /* ... */ });
```

`EventRouter::attach()` takes the same middleware list:

```cpp
#include <orbit/websocket/EventRouter.hpp>

struct Session { std::string user_id; };
orbit::websocket::EventRouter<Session> events;

// The handshake request, with what the middleware set on it, is readable
// during on_connect: initialise the session from it rather than from what
// the client sends later.
events.on_connect([](orbit::websocket::EventSocket<Session>& socket, const orbit::http::HttpRequest& req) {
    socket.session().user_id = req.user.value("sub", "");
});
events.attach(app, "/ws",
              {orbit::middleware::require_origin({"https://app.example.com"}),
               orbit::middleware::jwt_auth(secret)});
```

For `app.ws()` handlers, `ws.handshake_request()` gives the same request
while the handler runs (and null afterwards).

Handshakes that are not `GET`, lack `Connection: Upgrade`, carry a malformed
`Sec-WebSocket-Key`, or request a version other than 13 are answered with
`400 Bad Request`.

## Broadcasting

To build a chat server, you can store `WebSocketConnection` references or broadcast messages globally. The underlying sockets are non-blocking, making `send()` extremely fast.
