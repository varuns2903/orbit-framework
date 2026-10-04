#include <orbit/server/EventLoop.hpp>
#include <orbit/utils/Logger.hpp>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <orbit/network/KqueueProactor.hpp>
#elif defined(_WIN32)
#include <orbit/network/IocpProactor.hpp>
#else
#include <orbit/network/EpollProactor.hpp>
#include <orbit/network/IoUringProactor.hpp>
#endif
#include <iostream>
#include <orbit/network/PlatformSocket.hpp>
#include <thread>
#include <chrono>

namespace server {

EventLoop::EventLoop(Listener& listener, const routing::Router& router, const config::ServerConfig& config, network::TlsContext* tls_context, network::UdpSocket* quic_socket, QuicConnectionManager* quic_manager)
#if defined(__APPLE__) || defined(__FreeBSD__)
    : listener_(listener), 
      proactor_(std::make_unique<network::KqueueProactor>()),
#elif defined(_WIN32)
    : listener_(listener), 
      proactor_(std::make_unique<network::IocpProactor>()),
#else
    : listener_(listener), 
      proactor_(config.engine == config::EventEngine::Epoll ? 
               static_cast<std::unique_ptr<network::Proactor>>(std::make_unique<network::EpollProactor>()) : 
               static_cast<std::unique_ptr<network::Proactor>>(std::make_unique<network::IoUringProactor>())),
#endif
      thread_pool_(config.worker_threads), 
      connection_manager_(*proactor_, router, thread_pool_, timer_manager_, config.max_body_size, tls_context),
      quic_socket_(quic_socket),
      quic_manager_(quic_manager),
      max_connections_(config.max_connections) {
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(_WIN32)
    // The io_uring proactor accepts blocking sockets; epoll and kqueue use non-blocking ones.
    nonblocking_accepts_ = config.engine != config::EventEngine::IoUring;
#endif

    ConnectionTimeouts timeouts;
    timeouts.header = config.header_timeout;
    timeouts.keep_alive = config.keep_alive_timeout;
    timeouts.idle = config.idle_timeout;
    timeouts.websocket_idle = config.websocket_idle_timeout;
    connection_manager_.set_timeouts(timeouts);
    
    do_accept();

#ifdef ORBIT_ENABLE_HTTP3
    if (quic_socket_ && quic_manager_) {
        do_read_quic();
    }
#endif
}

void EventLoop::run() {
    LOG_INFO("Event loop started with ConnectionManager (HTTP Keep-Alive enabled)!");

    while (is_running_) {
        try {
            // Cap the wait so work signalled from outside the loop (e.g. a
            // shutdown request from a signal handler) is picked up promptly.
            int timeout_ms = timer_manager_.get_next_timeout();
            if (timeout_ms < 0 || timeout_ms > 200) timeout_ms = 200;
            // While accepting is paused, check back soon for a free slot.
            if (accept_paused_ && timeout_ms > 20) timeout_ms = 20;
            proactor_->run_once(timeout_ms);

            resume_accepting_if_ready();

            if (tick_hook_) tick_hook_();

            timer_manager_.handle_expired_timers([this](int fd) {
                connection_manager_.remove_connection(fd);
            });

            if (shutdown_requested_) drain_step();
            
            // If we are gracefully shutting down and have no active connections, exit
            if (!is_accepting_ && connection_manager_.get_connection_count() == 0) {
                is_running_ = false;
                LOG_INFO("All active connections drained. Shutting down completely.");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Error in event loop: " + std::string(e.what()));
        }
    }
    
    LOG_INFO("Event loop stopped. Shutting down...");
}

void EventLoop::stop() {
    is_running_ = false;
}

void EventLoop::request_shutdown(std::chrono::steady_clock::time_point deadline) {
    shutdown_deadline_ = deadline.time_since_epoch().count();
    shutdown_requested_ = true;
}

void EventLoop::drain_step() {
    if (!draining_) {
        draining_ = true;
        if (is_accepting_) stop_accepting();
    }
    // Repeated each iteration: connections finishing a response become idle.
    connection_manager_.notify_shutdown();

    auto deadline = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(shutdown_deadline_.load()));
    size_t remaining = connection_manager_.get_connection_count();
    if (remaining > 0 && std::chrono::steady_clock::now() >= deadline) {
        LOG_WARN("Shutdown deadline reached; closing " << remaining << " connection(s) that were still busy");
        is_running_ = false;
    }
}

void EventLoop::stop_accepting() {
    is_accepting_ = false;
    // Remove listener from proactor
    proactor_->remove(listener_.fd());
    // Refuse new connections outright: left open, they would queue in the
    // backlog and hang until the process exits. (On hot reload the new
    // process already listens on the same port via SO_REUSEPORT.)
    listener_.close();
    LOG_INFO("Event loop stopped accepting new connections. Waiting for active connections to drain...");
}

void EventLoop::do_accept() {
    proactor_->async_accept(listener_.fd(), [this](network::socket_t client_fd, sockaddr_in addr) {
        // Capture errno before anything else (logging included) can change it.
        const int accept_error = static_cast<int>(client_fd) < 0 ? network::get_last_socket_error() : 0;
        if (static_cast<int>(client_fd) >= 0) {
            on_accepted(client_fd, addr);
            accept_pending();
        } else {
#ifndef _WIN32
            if (accept_error == EMFILE || accept_error == ENFILE ||
                client_fd == -EMFILE || client_fd == -ENFILE) {
                // Out of descriptors: the listener stays readable, so re-arming
                // now would spin. Back off without blocking the loop.
                LOG_WARN("Accept failed: out of file descriptors; pausing accept for 100 ms");
                pause_accepting(std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
                return;
            }
#endif
            LOG_ERROR("Accept failed. FD: " << client_fd << " error: " << accept_error);
        }
        
        if (is_accepting_ && !accept_paused_) {
            do_accept();
        }
    });
}

bool EventLoop::at_connection_limit() const {
    return max_connections_ != 0 && connection_manager_.get_connection_count() >= max_connections_;
}

void EventLoop::pause_accepting(std::chrono::steady_clock::time_point until) {
    accept_paused_ = true;
    accept_resume_at_ = until;
}

void EventLoop::resume_accepting_if_ready() {
    if (!accept_paused_ || !is_accepting_) return;
    if (std::chrono::steady_clock::now() < accept_resume_at_ || at_connection_limit()) return;
    accept_paused_ = false;
    do_accept();
}

void EventLoop::on_accepted(network::socket_t client_fd, const sockaddr_in& addr) {
    LOG_DEBUG("Accepted new connection! FD: " << client_fd);
    // getpeername handles IPv6 too; the proactor's sockaddr_in is IPv4-only.
    std::string client_ip = network::peer_ip(client_fd);
    if (client_ip.empty()) {
        client_ip = network::format_ip(reinterpret_cast<const sockaddr*>(&addr));
    }
    network::Socket client(client_fd);
    connection_manager_.add_connection(std::move(client), client_ip);

    if (at_connection_limit()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_limit_warning_ >= std::chrono::seconds(10)) {
            last_limit_warning_ = now;
            LOG_WARN("max_connections (" << max_connections_ << ") reached; new connections wait in the backlog");
        }
        pause_accepting(now);
    }
}

void EventLoop::accept_pending() {
#ifndef _WIN32
    // One readiness event can stand for many queued connections. Take them
    // now (bounded, so a flood cannot starve established connections)
    // rather than one per loop iteration.
    for (int i = 0; i < 64 && !accept_paused_ && is_accepting_; ++i) {
        sockaddr_in addr{};
        network::socklen_t len = sizeof(addr);
        network::socket_t fd = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) {
            int err = errno;
            if (err == EMFILE || err == ENFILE) {
                LOG_WARN("Accept failed: out of file descriptors; pausing accept for 100 ms");
                pause_accepting(std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
            }
            return; // EAGAIN: queue drained; other errors: the next event retries
        }
        // Match the proactor's own accept (accept4 flags on Linux, explicit on BSD,
        // where accepted sockets inherit the listener's O_NONBLOCK).
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, nonblocking_accepts_ ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
        fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
        on_accepted(fd, addr);
    }
#endif
}

#ifdef ORBIT_ENABLE_HTTP3
void EventLoop::do_read_quic() {
    proactor_->async_wait_read(quic_socket_->fd(), [this]() {
        char buffer[65536];
        sockaddr_in sender_addr;
        while (true) {
            ssize_t bytes_read = quic_socket_->recv_from(buffer, sizeof(buffer), sender_addr);
            if (bytes_read > 0) {
                quic_manager_->on_packet_received(reinterpret_cast<uint8_t*>(buffer), static_cast<size_t>(bytes_read), sender_addr);
            } else {
                break;
            }
        }
        if (is_running_) {
            do_read_quic();
        }
    });
}
#endif

} // namespace server
