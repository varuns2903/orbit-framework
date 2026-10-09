#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/network/Proactor.hpp>
#include <liburing.h>
#include <atomic>
#include <mutex>
#include <functional>
#include <thread>
#include <vector>

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
    // IORING_FEAT_FAST_POLL (Linux 5.7+): a recv/send/accept that cannot
    // complete yet arms an internal poll instead of blocking an io-wq worker
    // thread. Without it, operations are punted to workers (IOSQE_ASYNC) so
    // they never stall the submitting thread (#169).
    bool fast_poll_{false};
    unsigned io_flags() const;

    // Only the loop thread (the one calling run_once) touches the ring.
    // Other threads -- handlers on the worker pool, user threads -- queue
    // their operation and wake the loop through an eventfd; the loop submits
    // the queue in one batch. Submitting from those threads made io_uring
    // run the operation's follow-up work on them (a sleeping pool worker),
    // and a thread that exits can have its I/O cancelled (#169, #220).
    // Set by the first run_once(), which also arms the wake-up read: a
    // request is driven by the thread that submitted it, so even that read
    // must come from the loop thread.
    std::atomic<std::thread::id> owner_{};
    int wake_fd_{-1};
    uint64_t wake_value_{0};
    std::mutex pending_mutex_;

    enum class OpType {
        WAKE,
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

    using Prep = std::function<void(struct io_uring_sqe*)>;
    struct Pending {
        IoContext* ctx; // nullptr for requests without a completion (cancel)
        Prep prep;
        unsigned flags;
    };
    std::vector<Pending> pending_;
    IoContext wake_ctx_; // type set to WAKE in the constructor

    // Submits now on the loop thread, otherwise queues and wakes the loop.
    void submit(IoContext* ctx, Prep prep, unsigned flags = 0);
    // With sq_mutex_ held: prepares one request (no io_uring_submit).
    void prepare_locked(IoContext* ctx, const Prep& prep, unsigned flags);
    void arm_wake_locked();
    void drain_pending();
};

} // namespace network
