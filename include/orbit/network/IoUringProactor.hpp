#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/network/Proactor.hpp>
#include <liburing.h>
#include <atomic>
#include <mutex>
#include <functional>

namespace orbit::network {

class IoUringProactor : public Proactor {
public:
    IoUringProactor(unsigned entries = 1024);
    ~IoUringProactor() override;

    void run_once(int timeout_ms) override;

    void async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) override;
    void async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) override;
    void async_wait_read(socket_t fd, std::function<void()> callback) override;
    void async_wait_write(socket_t fd, std::function<void()> callback) override;
    void async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) override;
    void async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) override;
    void async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) override;

    void remove(socket_t fd) override;

private:
    struct io_uring ring_;
    std::mutex sq_mutex_;
    // Contexts submitted to the kernel and not yet completed.
    std::atomic<long> inflight_{0};

    enum class OpType {
        READ,
        WRITE,
        WAIT_READ,
        WAIT_WRITE,
        SENDFILE,
        ACCEPT,
        CONNECT
    };

    struct IoContext {
        OpType type;
        int fd{-1};

        std::function<void(ssize_t)> io_cb;
        std::function<void()> wait_cb;
        std::function<void(socket_t, sockaddr_in)> accept_cb;
        std::function<void(int)> connect_cb;
        
        sockaddr_in client_addr{};
        socklen_t client_len{sizeof(sockaddr_in)};

        // For sendfile fallback
        int in_fd{-1};
        off_t offset{0};
        size_t count{0};
    };

    struct io_uring_sqe* get_sqe_safe();
};

} // namespace network
