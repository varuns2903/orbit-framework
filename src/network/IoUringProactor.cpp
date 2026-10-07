#include <orbit/network/IoUringProactor.hpp>
#include <stdexcept>
#include <chrono>
#include <iostream>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <poll.h>

namespace network {

IoUringProactor::IoUringProactor(unsigned entries) {
    if (io_uring_queue_init(entries, &ring_, 0) < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }
}

IoUringProactor::~IoUringProactor() {
    // Contexts still in flight hold callbacks that may own the last reference
    // to a Connection, and the kernel may still write into that connection's
    // buffers. Cancel every request and reap the completions, so the kernel is
    // done with the buffers, and release the contexts while the ring still
    // exists, since their destructors may call remove().
    {
        std::lock_guard<std::mutex> lock(sq_mutex_);
        if (struct io_uring_sqe* sqe = get_sqe_safe()) {
            io_uring_prep_cancel(sqe, nullptr, IORING_ASYNC_CANCEL_ANY);
            io_uring_sqe_set_data(sqe, nullptr);
            io_uring_submit(&ring_);
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (inflight_ > 0 && std::chrono::steady_clock::now() < deadline) {
        struct __kernel_timespec ts{0, 50 * 1000000};
        struct io_uring_cqe* cqe;
        if (io_uring_wait_cqe_timeout(&ring_, &cqe, &ts) < 0) continue;
        auto* ctx = static_cast<IoContext*>(io_uring_cqe_get_data(cqe));
        io_uring_cqe_seen(&ring_, cqe);
        if (ctx) {
            delete ctx;
            --inflight_;
        }
    }
    // Anything left was not cancelled in time; leaking it is safer than
    // freeing memory the kernel may still write to.
    io_uring_queue_exit(&ring_);
}

struct io_uring_sqe* IoUringProactor::get_sqe_safe() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

void IoUringProactor::async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::READ;
    ctx->fd = fd;
    ctx->io_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_recv(sqe, fd, buffer, size, 0);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WRITE;
    ctx->fd = fd;
    ctx->io_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_send(sqe, fd, buffer, size, 0);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_wait_read(socket_t fd, std::function<void()> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WAIT_READ;
    ctx->fd = fd;
    ctx->wait_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_poll_add(sqe, fd, POLLIN);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_wait_write(socket_t fd, std::function<void()> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WAIT_WRITE;
    ctx->fd = fd;
    ctx->wait_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_poll_add(sqe, fd, POLLOUT);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::SENDFILE;
    ctx->fd = out_fd;
    ctx->in_fd = in_fd;
    ctx->offset = offset;
    ctx->count = count;
    ctx->io_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        // Splice from regular file to TCP socket directly is illegal (fails with -EINVAL).
        // Instead, we use POLLOUT to wait for socket writability, then call sendfile().
        io_uring_prep_poll_add(sqe, out_fd, POLLOUT);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::ACCEPT;
    ctx->fd = fd;
    ctx->client_len = sizeof(sockaddr_in);
    ctx->accept_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_accept(sqe, fd, (struct sockaddr*)&ctx->client_addr, &ctx->client_len, SOCK_CLOEXEC);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_sqe_set_flags(sqe, IOSQE_ASYNC);
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::CONNECT;
    ctx->fd = fd;
    ctx->client_addr = addr;
    ctx->client_len = sizeof(addr);
    ctx->connect_cb = std::move(callback);

    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_connect(sqe, fd, (struct sockaddr*)&ctx->client_addr, ctx->client_len);
        io_uring_sqe_set_data(sqe, ctx);
        ++inflight_;
        io_uring_submit(&ring_);
    } else {
        delete ctx;
    }
}

void IoUringProactor::remove(socket_t fd) {
    // When the socket is closed, the kernel automatically cancels pending requests on it.
    // We can also explicitly submit a cancel request if desired.
    std::lock_guard<std::mutex> lock(sq_mutex_);
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (sqe) {
        io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data(sqe, nullptr); // no context needed for cancel cqe itself
        io_uring_submit(&ring_);
    }
}

void IoUringProactor::run_once(int timeout_ms) {
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    struct io_uring_cqe* cqe;
    
    // We don't hold the mutex here because io_uring_wait_cqe_timeout modifies CQ and doesn't conflict with SQ
    int ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
    if (ret < 0) {
        if (ret == -ETIME || ret == -EINTR || ret == -EAGAIN) return;
        throw std::runtime_error("io_uring_wait_cqe_timeout failed");
    }

    // Process all available CQEs
    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        count++;
        IoContext* ctx = static_cast<IoContext*>(io_uring_cqe_get_data(cqe));
        if (ctx) {
            if (cqe->res != -ECANCELED) {
                if (ctx->type == OpType::ACCEPT) {
                    if (cqe->res >= 0) {
                        ctx->accept_cb(cqe->res, ctx->client_addr);
                    } else if (cqe->res != -EAGAIN) {
                        ctx->accept_cb(-1, ctx->client_addr);
                    }
                } else if (ctx->type == OpType::CONNECT) {
                    ctx->connect_cb(cqe->res);
                } else if (ctx->type == OpType::SENDFILE) {
                    if (cqe->res > 0 && (cqe->res & POLLOUT)) {
                        ssize_t bytes = sendfile(ctx->fd, ctx->in_fd, &ctx->offset, ctx->count);
                        ctx->io_cb(bytes);
                    } else {
                        ctx->io_cb(-1);
                    }
                } else if (ctx->type == OpType::WAIT_READ || ctx->type == OpType::WAIT_WRITE) {
                    ctx->wait_cb();
                } else {
                    ctx->io_cb(cqe->res);
                }
            }
            delete ctx;
            --inflight_;
        }
    }

    io_uring_cq_advance(&ring_, count);
}

} // namespace network
