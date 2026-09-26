# WebSockets

Orbit has built-in support for RFC 6455 WebSockets. Because the core engine is asynchronous, a single server instance can hold millions of concurrent WebSocket connections efficiently without exhausting OS threads.

## Defining a WebSocket Route

Instead of `app.get()`, use `app.ws()` and pass a handler that accepts a `WebSocketConnection&`.

```cpp
app.ws("/chat", [](http::websocket::WebSocketConnection& ws) {
    
    // Register message callback
    ws.on_message([&ws](const std::string& msg, bool is_binary) {
        std::cout << "Received: " << msg << std::endl;
        
        // Echo it back
        ws.send("Echo: " + msg, is_binary);
    });

    // Register close callback
    ws.on_close([]() {
        std::cout << "Client disconnected" << std::endl;
    });
    
});
```

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
       {middleware::require_origin({"https://app.example.com"}),
        middleware::jwt_auth(secret)},
       [](http::websocket::WebSocketConnection& ws) { /* ... */ });
```

Handshakes that are not `GET`, lack `Connection: Upgrade`, carry a malformed
`Sec-WebSocket-Key`, or request a version other than 13 are answered with
`400 Bad Request`.

## Broadcasting

To build a chat server, you can store `WebSocketConnection` references or broadcast messages globally. The underlying sockets are non-blocking, making `send()` extremely fast.
