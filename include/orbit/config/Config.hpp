#pragma once
#include <string>
#include <cstdint>
#include <chrono>

namespace config {

enum class EventEngine {
    Epoll,
    IoUring
};

enum class HttpVersion {
    Http1_1,
    Http2,
    Http3
};

struct ServerConfig {
    uint16_t port{8080};
    // Address to listen on: "0.0.0.0" (all IPv4 interfaces), "::" (all IPv6
    // and IPv4, dual-stack), "127.0.0.1" / "::1" (local only), or a host name.
    std::string host{"0.0.0.0"};
    int backlog{0};              // Pending-connection queue length; 0 = SOMAXCONN
    // Open connections served at once; 0 = unlimited. Beyond it, new
    // connections wait in the listen backlog until a slot frees up.
    size_t max_connections{0};
    size_t worker_threads{4};
    std::string log_level{"INFO"};
    std::string static_dir{"./public"};
    size_t max_body_size{10485760}; // Default 10 MB limit
    std::string ssl_cert{""};
    std::string ssl_key{""};
    EventEngine engine{EventEngine::Epoll}; // Default to our new fast backend!
    HttpVersion http_version{HttpVersion::Http1_1}; // Default to HTTP/1.1

    // Connection timeouts. A value of zero disables that timeout.
    std::chrono::seconds header_timeout{10};         // From the first byte of a request until its headers are complete; not extended by further bytes
    std::chrono::seconds keep_alive_timeout{10};     // Idle time allowed between requests on a persistent connection
    std::chrono::seconds idle_timeout{30};           // Longest pause while receiving a body or writing a response
    std::chrono::seconds websocket_idle_timeout{0};  // Idle time on WebSocket/raw streams; 0 = never (use pings to detect dead peers)
    // Server-sent WebSocket pings; 0 = none. With websocket_idle_timeout set
    // longer than this, live peers (which answer with a pong) stay connected
    // and dead ones are closed.
    std::chrono::seconds websocket_ping_interval{0};

    // Request limits (HTTP/1.1). Oversized headers get 431, bodies 413.
    size_t max_header_bytes{8192};                   // Request line plus all headers
    size_t max_request_line{4096};                   // Method, target and version
    size_t max_headers{100};                         // Header fields per request
    size_t websocket_max_message_size{16 * 1024 * 1024}; // Per message, after decompression; larger closes with 1009
    // On SIGTERM/SIGINT (or App::shutdown()), how long in-flight requests may
    // finish before remaining connections are closed. A second signal stops at once.
    std::chrono::seconds shutdown_timeout{30};
    
    static ServerConfig parse(int argc, char* argv[]);
};

} // namespace config
