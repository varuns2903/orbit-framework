#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <functional>
#include <orbit/network/PlatformSocket.hpp>
#include <sys/types.h>

namespace orbit::network {

class Proactor {
public:
    virtual ~Proactor() = default;

    // Block and wait for completions
    virtual void run_once(int timeout_ms) = 0;

    // Asynchronous read
    virtual void async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t bytes_read)> callback) = 0;

    // Asynchronous write
    virtual void async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t bytes_written)> callback) = 0;
    
    // Asynchronous wait for read readiness
    virtual void async_wait_read(socket_t fd, std::function<void()> callback) = 0;

    // Asynchronous wait for write readiness
    virtual void async_wait_write(socket_t fd, std::function<void()> callback) = 0;
    
    // Asynchronous sendfile
    virtual void async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t bytes_written)> callback) = 0;

    // Asynchronous accept
    virtual void async_accept(socket_t fd, std::function<void(socket_t new_fd, sockaddr_in addr)> callback) = 0;

    // Asynchronous connect
    virtual void async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int status)> callback) = 0;

    // Cancel all operations and remove fd tracking
    virtual void remove(socket_t fd) = 0;
};

} // namespace network
