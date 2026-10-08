#pragma once
#include <orbit/legacy_namespaces.hpp>

#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #undef min
    #undef max
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>
    #include <netdb.h>
#endif

namespace orbit::network {

#ifdef _WIN32
    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
    constexpr int SOCKET_ERROR_VAL = SOCKET_ERROR;
    
    using socklen_t = int;
    using ssize_t = intptr_t;
    
    inline int get_last_socket_error() { return WSAGetLastError(); }
    inline bool is_would_block_error() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_FD = -1;
    constexpr int SOCKET_ERROR_VAL = -1;
    
    using socklen_t = ::socklen_t;
    using ssize_t = ::ssize_t;
    
    inline int get_last_socket_error() { return errno; }
    inline bool is_would_block_error() { return errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

    void initialize_platform_networking();
    void cleanup_platform_networking();
    void close_socket(socket_t fd);
    void set_non_blocking(socket_t fd);
    /// Shuts down both directions: the peer sees end-of-stream at once and
    /// pending receives complete, even before the descriptor is closed.
    void shutdown_socket(socket_t fd);

    /// Formats an IPv4 or IPv6 address; IPv4-mapped IPv6 addresses
    /// (::ffff:1.2.3.4, seen on dual-stack listeners) come back as plain
    /// IPv4. Returns "" for other families.
    std::string format_ip(const sockaddr* addr);

    /// The connected peer's IP address (see format_ip), or "" if unknown.
    std::string peer_ip(socket_t fd);
}
