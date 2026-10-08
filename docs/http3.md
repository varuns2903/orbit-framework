# HTTP/3 & QUIC (experimental)

> **Experimental.** HTTP/3 has only been tested against Orbit's own test suite (libcurl with
> HTTP/3). It has not been checked against browsers, other HTTP/3 implementations or
> production traffic. Serve production traffic over HTTP/1.1 or HTTP/2, and expect behaviour
> and configuration to change. See [Known Limitations](loopholes_and_drawbacks.md).

Orbit can serve HTTP/3 over QUIC (UDP) itself, using [ngtcp2](https://github.com/ngtcp2/ngtcp2)
and [nghttp3](https://github.com/ngtcp2/nghttp3), next to its HTTP/1.1 and HTTP/2 listener.

## Enabling HTTP/3

HTTP/3 needs:

- a build with `ORBIT_ENABLE_HTTP3=ON` (the default; with vcpkg, the `http3` feature), and
- a TLS certificate and key, because QUIC always uses TLS 1.3.

Then select HTTP/3 with `--http-version 3`, or `http_version` in code:

```bash
./my_app --port 8443 --ssl-cert server.crt --ssl-key server.key --http-version 3
```

```cpp
#include <orbit/server/App.hpp>

int main(int argc, char* argv[]) {
    auto cfg = orbit::config::ServerConfig::parse(argc, argv);
    cfg.http_version = orbit::config::HttpVersion::Http3;   // same as --http-version 3
    cfg.ssl_cert = "server.crt";
    cfg.ssl_key = "server.key";

    orbit::server::App app(cfg);
    app.get("/", []() -> std::string { return "Hello over HTTP/3!"; });
    app.listen();   // TCP: HTTP/1.1 and HTTP/2 over TLS; UDP, same port: HTTP/3
}
```

Without a certificate, Orbit logs a warning and starts without QUIC. Remember to open the
**UDP** port as well as the TCP port in firewalls and containers (see
[deployment.md](deployment.md)).

Try it with a curl built with HTTP/3 support:

```bash
curl --http3-only -k https://localhost:8443/
```

## How it works

When HTTP/3 is enabled, `App` binds a UDP socket on the same port as the TCP listener and
attaches a `QuicConnectionManager` to the **first** event loop. The manager runs the QUIC
handshake and connections (ngtcp2), and `QuicHttp3Session` decodes HTTP/3 requests (nghttp3).
Each request goes through the same `Router` as HTTP/1.1 and HTTP/2, so routes, middleware and
error handlers are shared. Responses support bodies, files and streamed chunks.

## Current limitations

- **Experimental:** see the note at the top of this page.
- QUIC runs on the first event loop only, even with `event_loops > 1`.
- No `Alt-Svc` advertisement from the TCP listener, so browsers won't discover HTTP/3 on
  their own.
- No 0-RTT, connection migration testing, or interoperability testing (e.g. the QUIC
  interop runner).
- Not covered by the performance work in
  [#183](https://github.com/varuns2903/orbit-framework/issues/183).
