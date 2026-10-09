#include <orbit/network/IoUringProactor.hpp>
#include <stdexcept>
#include <chrono>
#include <iostream>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <poll.h>
#include <unistd.h>

namespace orbit::network {

IoUringProactor::IoUringProactor(unsigned entries) {
    struct io_uring_params params{};
    if (io_uring_queue_init_params(entries, &ring_, &params) < 0) {
        throw std::runtime_error("Failed to initialize io_uring");
    }
    fast_poll_ = (params.features & IORING_FEAT_FAST_POLL) != 0;
    wake_ctx_.type = OpType::WAKE;
    // Blocking on purpose: io_uring honours O_NONBLOCK, and a non-blocking
    // eventfd read would complete at once with -EAGAIN instead of waiting.
    // Writers never block (that would take a counter near 2^64).
    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("Failed to create the io_uring wake-up eventfd");
    }
    // The wake-up read is armed by the loop thread on its first run_once():
    // a request's completion is driven by the thread that submitted it, and
    // the constructing thread may be busy elsewhere or gone.
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
        if (ctx && ctx != &wake_ctx_) {
            delete ctx;
            --inflight_;
        }
    }
    // Queued but never submitted: release their callbacks.
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& p : pending_) delete p.ctx;
        pending_.clear();
    }
    // Anything left was not cancelled in time; leaking it is safer than
    // freeing memory the kernel may still write to.
    io_uring_queue_exit(&ring_);
    if (wake_fd_ >= 0) close(wake_fd_);
}

struct io_uring_sqe* IoUringProactor::get_sqe_safe() {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

unsigned IoUringProactor::io_flags() const {
    // Forcing every socket operation onto io-wq ran ~100 kernel worker
    // threads under load and capped io_uring below epoll. With fast poll the
    // kernel waits for readiness itself, on the submitting ring.
    return fast_poll_ ? 0u : static_cast<unsigned>(IOSQE_ASYNC);
}

void IoUringProactor::arm_wake_locked() {
    if (struct io_uring_sqe* sqe = get_sqe_safe()) {
        io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
        io_uring_sqe_set_data(sqe, &wake_ctx_);
    }
}

void IoUringProactor::prepare_locked(IoContext* ctx, const Prep& prep, unsigned flags) {
    struct io_uring_sqe* sqe = get_sqe_safe();
    if (!sqe) {
        delete ctx;
        return;
    }
    prep(sqe);
    io_uring_sqe_set_data(sqe, ctx);
    if (flags) io_uring_sqe_set_flags(sqe, flags);
    if (ctx) ++inflight_;
}

void IoUringProactor::submit(IoContext* ctx, Prep prep, unsigned flags) {
    if (owner_.load(std::memory_order_acquire) == std::this_thread::get_id()) {
        std::lock_guard<std::mutex> lock(sq_mutex_);
        prepare_locked(ctx, prep, flags);
        io_uring_submit(&ring_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.push_back(Pending{ctx, std::move(prep), flags});
    }
    uint64_t one = 1;
    ssize_t n = write(wake_fd_, &one, sizeof(one));
    (void)n;
}

void IoUringProactor::drain_pending() {
    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_.empty()) return;
        batch.swap(pending_);
    }
    std::lock_guard<std::mutex> lock(sq_mutex_);
    for (auto& p : batch) prepare_locked(p.ctx, p.prep, p.flags);
    io_uring_submit(&ring_);
}

void IoUringProactor::async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::READ;
    ctx->fd = fd;
    ctx->io_cb = std::move(callback);
    submit(ctx, [fd, buffer, size](io_uring_sqe* sqe) { io_uring_prep_recv(sqe, fd, buffer, size, 0); }, io_flags());
}

void IoUringProactor::async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WRITE;
    ctx->fd = fd;
    ctx->io_cb = std::move(callback);
    submit(ctx, [fd, buffer, size](io_uring_sqe* sqe) { io_uring_prep_send(sqe, fd, buffer, size, 0); }, io_flags());
}

void IoUringProactor::async_wait_read(socket_t fd, std::function<void()> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WAIT_READ;
    ctx->fd = fd;
    ctx->wait_cb = std::move(callback);
    submit(ctx, [fd](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd, POLLIN); });
}

void IoUringProactor::async_wait_write(socket_t fd, std::function<void()> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::WAIT_WRITE;
    ctx->fd = fd;
    ctx->wait_cb = std::move(callback);
    submit(ctx, [fd](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, fd, POLLOUT); });
}

void IoUringProactor::async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::SENDFILE;
    ctx->fd = out_fd;
    ctx->in_fd = in_fd;
    ctx->offset = offset;
    ctx->count = count;
    ctx->io_cb = std::move(callback);
    // Splice from regular file to TCP socket directly is illegal (fails with -EINVAL).
    // Instead, we use POLLOUT to wait for socket writability, then call sendfile().
    submit(ctx, [out_fd](io_uring_sqe* sqe) { io_uring_prep_poll_add(sqe, out_fd, POLLOUT); });
}

void IoUringProactor::async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::ACCEPT;
    ctx->fd = fd;
    ctx->client_len = sizeof(sockaddr_in);
    ctx->accept_cb = std::move(callback);
    submit(ctx, [fd, ctx](io_uring_sqe* sqe) {
        io_uring_prep_accept(sqe, fd, (struct sockaddr*)&ctx->client_addr, &ctx->client_len, SOCK_CLOEXEC);
    }, io_flags());
}

void IoUringProactor::async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) {
    auto* ctx = new IoContext();
    ctx->type = OpType::CONNECT;
    ctx->fd = fd;
    ctx->client_addr = addr;
    ctx->client_len = sizeof(addr);
    ctx->connect_cb = std::move(callback);
    submit(ctx, [fd, ctx](io_uring_sqe* sqe) {
        io_uring_prep_connect(sqe, fd, (struct sockaddr*)&ctx->client_addr, ctx->client_len);
    });
}

void IoUringProactor::remove(socket_t fd) {
    // Not queued like other requests: the caller closes the socket next, and
    // the kernel may give its number to a new connection before the loop
    // drains the queue, so a late cancel-by-fd would cancel that connection's
    // requests. A cancel runs when it is submitted, so submitting it from
    // this thread is safe.
    std::vector<IoContext*> dropped;
    {
        // Requests queued for this socket and not yet submitted belong to the
        // connection being removed (it submits nothing after remove()).
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto keep = pending_.begin();
        for (auto it = pending_.begin(); it != pending_.end(); ++it) {
            if (it->ctx && it->ctx->fd == fd) {
                dropped.push_back(it->ctx);
            } else {
                if (keep != it) *keep = std::move(*it);
                ++keep;
            }
        }
        pending_.erase(keep, pending_.end());
    }
    for (IoContext* ctx : dropped) delete ctx;

    std::lock_guard<std::mutex> lock(sq_mutex_);
    if (struct io_uring_sqe* sqe = get_sqe_safe()) {
        io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data(sqe, nullptr); // no context needed for cancel cqe itself
        io_uring_submit(&ring_);
    }
}

void IoUringProactor::run_once(int timeout_ms) {
    // The thread that runs the loop owns the ring from now on.
    if (owner_.load(std::memory_order_relaxed) != std::this_thread::get_id()) {
        owner_.store(std::this_thread::get_id(), std::memory_order_release);
        std::lock_guard<std::mutex> lock(sq_mutex_);
        arm_wake_locked();
        io_uring_submit(&ring_);
    }
    // Requests queued by other threads since the last iteration.
    drain_pending();

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
    bool woken = false;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        count++;
        IoContext* ctx = static_cast<IoContext*>(io_uring_cqe_get_data(cqe));
        if (ctx == &wake_ctx_) {
            woken = true; // re-armed below; its value only counts wake-ups
            continue;
        }
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

    if (woken) {
        std::lock_guard<std::mutex> lock(sq_mutex_);
        arm_wake_locked();
        io_uring_submit(&ring_);
    }
    drain_pending();
}

} // namespace network
