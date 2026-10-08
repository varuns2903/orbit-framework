#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/network/PlatformSocket.hpp>

namespace orbit::network {

class Socket {
public:
    // Create a new socket
    Socket();
    
    // Wrap an existing socket file descriptor
    explicit Socket(socket_t fd);
    
    // Destructor: Closes the socket to prevent leaks (RAII)
    ~Socket();

    // Delete copy semantics to prevent double-closing
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Allow move semantics
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    socket_t fd() const;
    bool is_valid() const;
    void close();
    
    // Convert socket to non-blocking mode
    void set_non_blocking();

private:
    socket_t fd_;
};

} // namespace network
