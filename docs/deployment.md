# Production Deployment

Deploying Orbit applications to production is straightforward thanks to its minimal runtime footprint. The recommended approach is to use **Docker** to containerize the compiled binary along with its dynamic library dependencies.

## 1. Using the Official Dockerfile

Orbit provides a multi-stage `Dockerfile` in the root repository. 

The build works in two stages:
1. **Builder Stage**: Installs the compiler, CMake, and development headers (`libpq-dev`, `libhiredis-dev`, etc.). It also fetches and compiles `quictls`, `ngtcp2`, and `nghttp3` for HTTP/3 support.
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
