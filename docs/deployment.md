# Production Deployment

Deploying Orbit applications to production is straightforward thanks to its minimal runtime footprint. The recommended approach is to use **Docker** to containerize the compiled binary along with its dynamic library dependencies.

## 1. Using the Official Dockerfile

Orbit provides a multi-stage `Dockerfile` in the root repository. 

The build works in two stages:
1. **Builder Stage**: Installs the compiler, CMake, and development headers (`libpq-dev`, `libmariadb-dev`, etc.). It also fetches and compiles `quictls`, `ngtcp2`, and `nghttp3` for HTTP/3 support.
2. **Runtime Stage**: Copies *only* the compiled binaries and the required runtime shared libraries (`.so` files) into a clean, lightweight Ubuntu image.

### Building the Image

```bash
# Build the production image tagged as 'orbit-app'
docker build -t orbit-app .
```

### Running the Container

When running the container, remember to expose the necessary TCP and UDP ports (UDP is strictly required if you have HTTP/3 enabled).

```bash
docker run -d \
  --name my-orbit-server \
  -p 8080:8080 \
  -p 8443:8443 \
  -p 8443:8443/udp \
  --restart unless-stopped \
  orbit-app
```

## 2. Docker Compose

For complex applications that require databases (PostgreSQL, Redis), `docker-compose` is the most robust way to manage the stack.

Review the `docker-compose.yml` included in the root directory:

```yaml
version: '3.8'

services:
  orbit:
    build: .
    ports:
      - "8080:8080"     # HTTP
      - "8443:8443"     # HTTPS (TCP)
      - "8443:8443/udp" # HTTP/3 (QUIC)
    environment:
      - PG_HOST=postgres
      - PG_USER=orbit_user
      - PG_PASSWORD=orbit_pass
      - REDIS_HOST=redis
    depends_on:
      postgres:
        condition: service_healthy
      redis:
        condition: service_healthy

  postgres:
    image: postgres:15-alpine
    # ... healthchecks ...

  redis:
    image: redis:7-alpine
    # ... healthchecks ...
```

Simply run:
```bash
docker-compose up -d --build
```

## Listening Address and Connection Limits

| Field | CLI | Default | Meaning |
|---|---|---|---|
| `host` | `-b, --bind` | `0.0.0.0` | Address to listen on. `0.0.0.0` = all IPv4 interfaces, `::` = all IPv6 **and** IPv4 (dual-stack), `127.0.0.1` / `::1` = local only. Host names such as `localhost` are resolved. |
| `backlog` | `--backlog` | `0` (`SOMAXCONN`) | Length of the kernel's pending-connection queue. |
| `max_connections` | `--max-connections` | `0` (unlimited) | Connections served at once. When reached, the server stops accepting; new connections wait in the backlog until a slot frees up. |

Behind a reverse proxy on the same host, bind to `127.0.0.1` so the app port is
not reachable from outside:

```cpp
config::ServerConfig cfg;
cfg.host = "127.0.0.1";
cfg.max_connections = 10000;
```

On a dual-stack listener, IPv4 clients appear in `req.client_ip` as plain
addresses (`203.0.113.7`), not as `::ffff:203.0.113.7`.

## Graceful Shutdown and Health Checks

On `SIGTERM` or `SIGINT` (or `app.shutdown()`), the server drains instead of
dropping connections:

1. It closes the listening socket; new connections are refused, so load
   balancers move on at once.
2. Idle keep-alive connections are closed.
3. Requests in progress finish; their responses carry `Connection: close`.
   HTTP/2 clients get `GOAWAY`; WebSocket clients get close code `1001`
   (going away).
4. Whatever is still busy after `shutdown_timeout` (default 30 s) is closed,
   and `listen()` returns.

A second signal stops immediately. `app.stop()` also stops immediately.

```cpp
config::ServerConfig cfg;
cfg.shutdown_timeout = std::chrono::seconds(20); // keep below Kubernetes' terminationGracePeriodSeconds
server::App app(cfg);
app.enable_health_checks(); // GET /healthz (liveness), GET /readyz (readiness)
```

`/healthz` answers `200` while the process runs. `/readyz` answers `200` while
serving and `503` once draining has begun. In Kubernetes:

```yaml
livenessProbe:
  httpGet: { path: /healthz, port: 8080 }
readinessProbe:
  httpGet: { path: /readyz, port: 8080 }
terminationGracePeriodSeconds: 30
```

## Connection Timeouts

`ServerConfig` has one timeout per phase of a connection. A value of `0`
disables that timeout.

| Field | Default | Applies to |
|---|---|---|
| `header_timeout` | 10 s | From the first byte of a request until its headers are complete. Later bytes do **not** extend it, so a client cannot hold a connection by trickling header bytes. |
| `keep_alive_timeout` | 10 s | Idle time between requests on a persistent connection. |
| `idle_timeout` | 30 s | Longest pause while receiving a request body or writing a response. |
| `websocket_idle_timeout` | 0 (off) | Idle time on WebSocket and raw-stream connections. |

No timeout runs while a handler is executing, so slow handlers and
Server-Sent Events streams are not cut off. WebSocket connections stay open while
quiet; to drop dead peers, set `websocket_idle_timeout` and have clients send
pings.

```cpp
config::ServerConfig cfg;
cfg.header_timeout = std::chrono::seconds(5);
cfg.keep_alive_timeout = std::chrono::seconds(60);
cfg.websocket_idle_timeout = std::chrono::seconds(120);
```

## 3. Reverse Proxies (NGINX / HAProxy)

While Orbit is perfectly capable of being exposed directly to the public internet (and features its own Load Balancing and Proxy middlewares), you may wish to run it behind an enterprise reverse proxy like NGINX.

> **Warning**: NGINX does not natively proxy HTTP/3 (QUIC) to backend servers. If you put Orbit behind NGINX, you will lose end-to-end QUIC termination unless configured explicitly using NGINX's experimental QUIC routing.

If using NGINX, ensure you configure it to pass standard HTTP headers to Orbit:

```nginx
server {
    listen 80;
    server_name api.my-orbit-app.com;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        # Required for Orbit WebSockets!
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_http_version 1.1;
    }
}
```

## 4. Bare Metal / VPS Deployment (systemd)

To deploy without Docker on a Linux VPS (Ubuntu/Debian):

1. **Install Runtime Dependencies**:
   ```bash
   sudo apt-get install libpq5 zlib1g libnghttp2-14 liburing2
   ```
2. **Copy your compiled binary** (e.g., `basic_server`) to `/usr/local/bin/orbit-server`.
3. **Create a systemd service file** `/etc/systemd/system/orbit.service`:

   ```ini
   [Unit]
   Description=Orbit Web Server
   After=network.target

   [Service]
   Type=simple
   User=www-data
   ExecStart=/usr/local/bin/orbit-server
   Restart=always
   RestartSec=3
   LimitNOFILE=65535

   [Install]
   WantedBy=multi-user.target
   ```

4. **Enable and Start**:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl enable orbit
   sudo systemctl start orbit
   ```
