# API Gateway, Proxy & Load Balancing

Orbit is not just a web framework; it also acts as a high-performance Reverse Proxy and Load Balancer. It supports HTTP connection pooling and TLS Session Reuse for ultra-low latency routing.

## Reverse Proxy

You can proxy any path to an upstream server. Orbit automatically handles chunked transfer encodings, headers (`X-Forwarded-For`), and Keep-Alive multiplexing.

```cpp
#include "middleware/Proxy.hpp"

// Proxy all /api requests to an upstream backend
orbit::middleware::ProxyOptions proxy_opts;
proxy_opts.strip_prefix = true; // strips "/api" before forwarding

app.use("/api", orbit::middleware::Proxy::create("http://localhost:8081", proxy_opts));
```

### Proxy Options

| Field | Default | Meaning |
|---|---|---|
| `target_host`, `target_port` | — | Upstream address |
| `strip_prefix` | `""` | Removed from the front of the path before forwarding; the query string is kept |
| `use_tls` | `false` | Connect over TLS (also implied by port 443) |
| `verify_tls` | `true` | Verify the upstream certificate chain **and** that it matches `target_host` |
| `ca_file` | `""` | PEM bundle of CAs to trust instead of the system store (e.g. an internal CA) |
| `trust_forwarded_headers` | `false` | Keep and extend client-supplied `X-Forwarded-For` / `X-Real-IP` / `X-Forwarded-Host` |

Hop-by-hop headers (`Connection`, `Keep-Alive`, `Transfer-Encoding`, `TE`,
`Upgrade`, `Proxy-*`, and any header named in `Connection`) are not forwarded;
`Host` and `Content-Length` are set by the proxy.

By default the proxy assumes it faces clients directly, so any
`X-Forwarded-For` a client sends is discarded and replaced with the client's
address. Set `trust_forwarded_headers` only when every request reaches Orbit
through another proxy you control; the client address is then appended.

`LoadBalancerOptions` has the same `verify_tls`, `ca_file` and
`trust_forwarded_headers` fields, and `TargetNode` has `use_tls`.

## Load Balancing

For distributing traffic across multiple backend servers, use the `LoadBalancer` middleware. It supports connection pooling and TLS multiplexing out of the box.

```cpp
orbit::middleware::LoadBalancerOptions lb_opts;
lb_opts.strip_prefix = true;

auto lb = orbit::middleware::LoadBalancer::create({
    "http://localhost:8081",
    "http://localhost:8082",
    "https://api.secure-backend.com" // TLS Session Reuse is natively supported!
}, lb_opts);

app.use("/lb-api", lb);
```

## Connection Pooling (Zero-Overhead)

Orbit maintains a global `ConnectionPool` mapped by `Host:Port`. When a Proxy or Load Balancer receives a request, it grabs a dormant TCP or TLS connection instead of performing a new Handshake. If no connections are available, it spawns a new one non-blockingly via the `Proactor`.
