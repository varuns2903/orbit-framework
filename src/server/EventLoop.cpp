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
      quic_manager_(quic_manager) {
    
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
            proactor_->run_once(timer_manager_.get_next_timeout());

            timer_manager_.handle_expired_timers([this](int fd) {
                connection_manager_.remove_connection(fd);
            });
            
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

void EventLoop::stop_accepting() {
    is_accepting_ = false;
    // Remove listener from proactor
    proactor_->remove(listener_.fd());
    LOG_INFO("Event loop stopped accepting new connections. Waiting for active connections to drain...");
}

void EventLoop::do_accept() {
    proactor_->async_accept(listener_.fd(), [this](int client_fd, sockaddr_in addr) {
        if (client_fd >= 0) {
            std::cout << "Accepted new connection! FD: " << client_fd << std::endl;
            std::string client_ip = inet_ntoa(addr.sin_addr);
            network::Socket client(client_fd);
            connection_manager_.add_connection(std::move(client), client_ip);
        } else {
            LOG_ERROR("Accept failed. FD: " << client_fd);
            if (client_fd == -EMFILE || client_fd == -ENFILE) {
                // Wait briefly before retrying if we hit fd limits
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        if (is_accepting_) {
            do_accept();
        }
    });
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
