#include <orbit/network/EpollProactor.hpp>
#include <orbit/utils/Logger.hpp>
#include <stdexcept>
#include <unistd.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <errno.h>

namespace orbit::network {

EpollProactor::EpollProactor() : events_(1024) {
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
}

EpollProactor::~EpollProactor() {
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
    close(epoll_fd_);
}

void EpollProactor::update_epoll(Context& ctx) {
    uint32_t events = 0;
    if (ctx.reading || ctx.accepting || ctx.waiting_read) events |= EPOLLIN;
    if (ctx.writing || ctx.sendfile_in_progress || ctx.connecting || ctx.waiting_write) events |= EPOLLOUT;

    events |= EPOLLONESHOT; // Always use oneshot to prevent duplicate events on same fd

    epoll_event ev{};
    ev.events = events;
    ev.data.fd = ctx.fd;

    if (!ctx.tracked && events != EPOLLONESHOT) {
        ctx.tracked = true;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ctx.fd, &ev) == -1) {
            LOG_ERROR("epoll_ctl ADD failed for fd " << ctx.fd);
        }
    } else if (ctx.tracked) {
        if (events == EPOLLONESHOT) {
            // No events to track, remove
            ctx.tracked = false;
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, ctx.fd, nullptr);
        } else {
            // Update
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, ctx.fd, &ev) == -1) {
                LOG_ERROR("epoll_ctl MOD failed for fd " << ctx.fd);
            }
        }
    }
}

void EpollProactor::async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.reading = true;
    ctx.read_buf = buffer;
    ctx.read_size = size;
    ctx.read_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.writing = true;
    ctx.write_buf = buffer;
    ctx.write_size = size;
    ctx.write_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_wait_read(socket_t fd, std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.waiting_read = true;
    ctx.wait_read_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_wait_write(socket_t fd, std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.waiting_write = true;
    ctx.wait_write_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[out_fd];
    ctx.fd = out_fd;
    ctx.sendfile_in_progress = true;
    ctx.sendfile_in_fd = in_fd;
    ctx.sendfile_offset = offset;
    ctx.sendfile_count = count;
    ctx.sendfile_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.accepting = true;
    ctx.accept_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) {
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        callback(0); // Immediately connected
        return;
    }
    if (errno != EINPROGRESS) {
        callback(-errno); // Immediate failure
        return;
    }

    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto& ctx = contexts_[fd];
    ctx.fd = fd;
    ctx.connecting = true;
    ctx.connect_cb = std::move(callback);
    update_epoll(ctx);
}

void EpollProactor::remove(socket_t fd) {
    std::lock_guard<std::mutex> lock(ctx_mutex_);
    auto it = contexts_.find(fd);
    if (it != contexts_.end()) {
        if (it->second.tracked) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        }
        contexts_.erase(it);
    }
}

void EpollProactor::run_once(int timeout_ms) {
    int nfds = epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_ms);
    if (nfds == -1) {
        if (errno == EINTR) return;
        throw std::runtime_error("epoll_wait failed");
    }

    // Capture triggered events locally to avoid holding the lock during callbacks
    struct Trigger {
        std::function<void()> callback;
    };
    std::vector<Trigger> triggers;

    {
        std::lock_guard<std::mutex> lock(ctx_mutex_);
        for (int i = 0; i < nfds; ++i) {
            int fd = events_[i].data.fd;
            auto it = contexts_.find(fd);
            if (it == contexts_.end()) continue;
            
            auto& ctx = it->second;
            uint32_t ev = events_[i].events;

            if (ev & EPOLLIN) {
                if (ctx.accepting) {
                    ctx.accepting = false;
                    auto cb = std::move(ctx.accept_cb);
                    triggers.push_back({[fd, cb]() {
                        sockaddr_in client_addr{};
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = accept4(fd, (struct sockaddr*)&client_addr, &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (client_fd >= 0) {
                            cb(client_fd, client_addr);
                        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            cb(-1, client_addr); // Error
                        }
                    }});
                } else if (ctx.reading) {
                    ctx.reading = false;
                    auto cb = std::move(ctx.read_cb);
                    void* buf = ctx.read_buf;
                    size_t sz = ctx.read_size;
                    triggers.push_back({[fd, buf, sz, cb]() {
                        ssize_t bytes = recv(fd, buf, sz, 0);
                        cb(bytes);
                    }});
                } else if (ctx.waiting_read) {
                    ctx.waiting_read = false;
                    auto cb = std::move(ctx.wait_read_cb);
                    triggers.push_back({cb});
                }
            }
            
            if (ev & EPOLLOUT) {
                if (ctx.connecting) {
                    ctx.connecting = false;
                    auto cb = std::move(ctx.connect_cb);
                    triggers.push_back({[fd, cb]() {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
                            cb(-errno);
                        } else {
                            cb(-error);
                        }
                    }});
                } else if (ctx.writing) {
                    ctx.writing = false;
                    auto cb = std::move(ctx.write_cb);
                    const void* buf = ctx.write_buf;
                    size_t sz = ctx.write_size;
                    triggers.push_back({[fd, buf, sz, cb]() {
                        ssize_t bytes = send(fd, buf, sz, 0);
                        cb(bytes);
                    }});
                } else if (ctx.sendfile_in_progress) {
                    ctx.sendfile_in_progress = false;
                    auto cb = std::move(ctx.sendfile_cb);
                    int in_fd = ctx.sendfile_in_fd;
                    off_t offset = ctx.sendfile_offset;
                    size_t count = ctx.sendfile_count;
                    triggers.push_back({[fd, in_fd, offset, count, cb]() mutable {
                        ssize_t bytes = sendfile(fd, in_fd, &offset, count);
                        cb(bytes);
                    }});
                } else if (ctx.waiting_write) {
                    ctx.waiting_write = false;
                    auto cb = std::move(ctx.wait_write_cb);
                    triggers.push_back({cb});
                }
            }
            
            // Re-arm remaining oneshots if needed
            update_epoll(ctx);
        }
    }

    // Execute callbacks outside the lock
    for (auto& trigger : triggers) {
        trigger.callback();
    }
}

} // namespace network
