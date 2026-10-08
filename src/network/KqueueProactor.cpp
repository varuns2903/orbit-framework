#if defined(__APPLE__) || defined(__FreeBSD__)
#include <orbit/network/KqueueProactor.hpp>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <cerrno>
#include <functional>

namespace orbit::network {

KqueueProactor::KqueueProactor() : events_(64) {
    kq_fd_ = kqueue();
    if (kq_fd_ < 0) {
        throw std::runtime_error("Failed to create kqueue");
    }
}

KqueueProactor::~KqueueProactor() {
    // Pending callbacks may hold the last reference to objects whose
    // destructors call remove() (e.g. a proxied request). Release them while
    // ctx_mutex_ still exists: destroying contexts_ as a member happens after
    // the mutex is gone, and macOS aborts on locking a destroyed mutex.
    std::unordered_map<int, Context> pending;
    {
        std::lock_guard<std::mutex> lock(ctx_mutex_);
        pending.swap(contexts_);
    }
    pending.clear();
    if (kq_fd_ >= 0) close(kq_fd_);
}

void KqueueProactor::update_kqueue(Context& ctx) {
    std::vector<struct kevent> changes;
    
    // EVFILT_READ
    bool needs_read = ctx.reading || ctx.accepting || ctx.waiting_read;
    struct kevent ev_read;
    EV_SET(&ev_read, ctx.fd, EVFILT_READ, needs_read ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    changes.push_back(ev_read);
    
    // EVFILT_WRITE
    bool needs_write = ctx.writing || ctx.connecting || ctx.waiting_write || ctx.sendfile_in_progress;
    struct kevent ev_write;
    EV_SET(&ev_write, ctx.fd, EVFILT_WRITE, needs_write ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    changes.push_back(ev_write);

    // Apply each change on its own. With no event list, kevent() stops at the
    // first failing change and returns -1: deleting a filter that was never
    // added (ENOENT) used to silently drop the following EV_ADD, so e.g. the
    // write filter for an outgoing connect was never registered.
    for (auto& change : changes) {
        kevent(kq_fd_, &change, 1, nullptr, 0, nullptr);
    }
    ctx.tracked = true;
}

void KqueueProactor::remove(socket_t fd) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto it = contexts_.find(fd);
    if (it != contexts_.end()) {
        struct kevent changes[2];
        EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        // Separately, for the same reason as in update_kqueue().
        kevent(kq_fd_, &changes[0], 1, nullptr, 0, nullptr);
        kevent(kq_fd_, &changes[1], 1, nullptr, 0, nullptr);
        contexts_.erase(it);
    }
}

void KqueueProactor::run_once(int timeout_ms) {
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    
    int n = kevent(kq_fd_, nullptr, 0, events_.data(), events_.size(), timeout_ms >= 0 ? &ts : nullptr);
    if (n < 0) return;

    std::vector<struct kevent> ready_events(events_.begin(), events_.begin() + n);

    for (const auto& ev : ready_events) {
        handle_event(ev);
    }
}

void KqueueProactor::handle_event(const struct kevent& event) {
    const int fd = static_cast<int>(event.ident);

    // Take everything needed out of the context under the lock, then do the
    // I/O and run the callback without it, as the epoll proactor does.
    // Calling ctx->read_cb in place was unsafe: callbacks usually re-arm the
    // same operation, and assigning the new std::function destroyed the one
    // still executing (and the shared_ptr it captured). The context pointer
    // could also be invalidated by another thread growing contexts_.
    std::function<void()> action;
    {
        std::lock_guard<std::mutex> lock(ctx_mutex_);
        auto it = contexts_.find(fd);
        if (it == contexts_.end()) return;
        Context& ctx = it->second;

        if (event.filter == EVFILT_READ) {
            if (ctx.accepting) {
                ctx.accepting = false;
                auto cb = std::move(ctx.accept_cb);
                update_kqueue(ctx);
                action = [fd, cb = std::move(cb)]() {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int new_fd = ::accept(fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
                    if (new_fd >= 0) {
                        // Match accept4(SOCK_NONBLOCK | SOCK_CLOEXEC) on Linux.
                        fcntl(new_fd, F_SETFL, fcntl(new_fd, F_GETFL, 0) | O_NONBLOCK);
                        fcntl(new_fd, F_SETFD, fcntl(new_fd, F_GETFD, 0) | FD_CLOEXEC);
                    }
                    if (cb) cb(new_fd, client_addr);
                };
            } else if (ctx.reading) {
                ctx.reading = false;
                auto cb = std::move(ctx.read_cb);
                void* buf = ctx.read_buf;
                size_t size = ctx.read_size;
                update_kqueue(ctx);
                action = [fd, buf, size, cb = std::move(cb)]() {
                    ssize_t n = ::read(fd, buf, size);
                    if (cb) cb(n);
                };
            } else if (ctx.waiting_read) {
                ctx.waiting_read = false;
                auto cb = std::move(ctx.wait_read_cb);
                update_kqueue(ctx);
                action = std::move(cb);
            }
        } else if (event.filter == EVFILT_WRITE) {
            if (ctx.connecting) {
                ctx.connecting = false;
                auto cb = std::move(ctx.connect_cb);
                update_kqueue(ctx);
                action = [fd, cb = std::move(cb)]() {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
                    if (cb) cb(err);
                };
            } else if (ctx.writing) {
                ctx.writing = false;
                auto cb = std::move(ctx.write_cb);
                const void* buf = ctx.write_buf;
                size_t size = ctx.write_size;
                update_kqueue(ctx);
                action = [fd, buf, size, cb = std::move(cb)]() {
                    ssize_t n = ::write(fd, buf, size);
                    if (cb) cb(n);
                };
            } else if (ctx.waiting_write) {
                ctx.waiting_write = false;
                auto cb = std::move(ctx.wait_write_cb);
                update_kqueue(ctx);
                action = std::move(cb);
            } else if (ctx.sendfile_in_progress) {
                ctx.sendfile_in_progress = false;
                auto cb = std::move(ctx.sendfile_cb);
                int in_fd = ctx.sendfile_in_fd;
                off_t offset = ctx.sendfile_offset;
                size_t count = ctx.sendfile_count;
                update_kqueue(ctx);
                // One call per readiness event; the caller re-arms for the rest,
                // as with Linux sendfile().
                action = [fd, in_fd, offset, count, cb = std::move(cb)]() {
                    off_t len = static_cast<off_t>(count);
                    int ret = sendfile(in_fd, fd, offset, &len, nullptr, 0);
                    bool progressed = (ret == 0) || ((errno == EAGAIN || errno == EINTR) && len > 0);
                    if (cb) cb(progressed ? static_cast<ssize_t>(len) : -1);
                };
            }
        }
    }

    if (action) action();
}

// Setup methods (they mostly just set the state and call update_kqueue)
void KqueueProactor::async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.reading = true;
    ctx.read_buf = buffer;
    ctx.read_size = size;
    ctx.read_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.writing = true;
    ctx.write_buf = buffer;
    ctx.write_size = size;
    ctx.write_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_wait_read(socket_t fd, std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.waiting_read = true;
    ctx.wait_read_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_wait_write(socket_t fd, std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.waiting_write = true;
    ctx.wait_write_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[out_fd];
    ctx.fd = out_fd;
    ctx.sendfile_in_progress = true;
    ctx.sendfile_in_fd = in_fd;
    ctx.sendfile_offset = offset;
    ctx.sendfile_count = count;
    ctx.sendfile_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.accepting = true;
    ctx.accept_cb = std::move(callback);
    update_kqueue(ctx);
}

void KqueueProactor::async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) {
    int ret = ::connect(fd, (const struct sockaddr*)&addr, sizeof(addr));
    if (ret == 0) {
        if (callback) callback(0);
        return;
    }
    if (errno != EINPROGRESS) {
        if (callback) callback(errno);
        return;
    }
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.connecting = true;
    ctx.connect_cb = std::move(callback);
    update_kqueue(ctx);
}

} // namespace network
#endif
