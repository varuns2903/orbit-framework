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
    
    static ServerConfig parse(int argc, char* argv[]);
};

} // namespace config
