# HTTP/3 & QUIC Support

Orbit is built with forward-compatibility in mind. It fully supports **HTTP/3 over QUIC (UDP)** natively without requiring an external proxy like NGINX.

## Enabling HTTP/3

To enable HTTP/3, simply pass the `--engine` flag and ensure you have valid TLS Certificates (HTTP/3 mandates TLS 1.3).

```cpp
#include "server/App.hpp"
#include "network/TlsContext.hpp"

int main() {
    orbit::config::ServerConfig cfg;
    cfg.port = 443;
    
    // HTTP/3 requires TLS 1.3
    orbit::network::TlsContext tls_ctx("server.crt", "server.key");

    App app(cfg, &tls_ctx);

    app.get("/", [](HttpRequest& req, std::shared_ptr<ResponseWriter> res) {
        res->send(HttpResponse().send("Hello over HTTP/3!"));
    });

    // Binds TCP 443 (HTTP/1 & HTTP/2) and UDP 443 (HTTP/3 QUIC)
    app.listen(443, []() {
        std::cout << "Server active on TCP and UDP" << std::endl;
    });

    return 0;
}
```

## How it Works

When initialized with a TLS Context, Orbit spins up a highly optimized `UdpSocket` running in parallel to the `TcpListener`. The `QuicConnectionManager` demultiplexes QUIC streams and passes them into the exact same `Router` that handles HTTP/1 and HTTP/2, providing a completely seamless developer experience.
