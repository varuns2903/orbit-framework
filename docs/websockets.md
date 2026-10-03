# WebSockets

Orbit has built-in support for RFC 6455 WebSockets. Because the core engine is asynchronous, a single server instance can hold millions of concurrent WebSocket connections efficiently without exhausting OS threads.

## Defining a WebSocket Route

Instead of `app.get()`, use `app.ws()` and pass a handler that accepts a `WebSocketConnection&`.

```cpp
app.ws("/chat", [](http::websocket::WebSocketConnection& ws) {
    
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
app.ws("/chat", [](http::websocket::WebSocketConnection& ws) {
    ws.set_max_message_size(64 * 1024); // 64 KiB
    ws.on_message([&ws](const std::string& msg) { ws.send(msg); });
});
```

## Broadcasting

To build a chat server, you can store `WebSocketConnection` references or broadcast messages globally. The underlying sockets are non-blocking, making `send()` extremely fast.
