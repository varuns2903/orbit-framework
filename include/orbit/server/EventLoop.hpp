#pragma once
#include <orbit/network/Proactor.hpp>
#include <orbit/server/Listener.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/server/ConnectionManager.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/server/TimerManager.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/network/TlsContext.hpp>
#include <orbit/network/UdpSocket.hpp>
#ifdef ORBIT_ENABLE_HTTP3
#include <orbit/server/QuicConnectionManager.hpp>
#else
namespace server { class QuicConnectionManager; }
#endif
#include <atomic>

namespace server {

/**
 * @brief Manages the server's event loop, handling I/O operations and dispatching tasks.
 */
class EventLoop {
public:
    /**
     * @brief Constructs an EventLoop.
     * @param listener The server listener.
     * @param router The router instance.
     * @param config The server configuration.
     * @param tls_context The TLS context (optional).
     * @param quic_socket The QUIC UDP socket (optional).
     * @param quic_manager The QUIC connection manager (optional).
     */
    EventLoop(Listener& listener, const routing::Router& router, const config::ServerConfig& config, network::TlsContext* tls_context = nullptr, network::UdpSocket* quic_socket = nullptr, QuicConnectionManager* quic_manager = nullptr);
    
    /**
     * @brief Starts the event loop.
     * 
     * @code
     * EventLoop loop(listener, router, config);
     * loop.run();
     * @endcode
     */
    void run();
    
    /**
     * @brief Stops the event loop.
     */
    void stop();
    
    /**
     * @brief Stops accepting new connections but continues processing existing ones.
     */
    void stop_accepting();

    /**
     * @brief Gets the thread pool.
     * @return Reference to the thread pool.
     */
    concurrency::ThreadPool& get_thread_pool() { return thread_pool_; }

private:
    void do_accept();
    void do_read_quic();

    Listener& listener_;
    TimerManager timer_manager_;
    std::unique_ptr<network::Proactor> proactor_;
    concurrency::ThreadPool thread_pool_;
    ConnectionManager connection_manager_;
    network::UdpSocket* quic_socket_;
    QuicConnectionManager* quic_manager_;
    
    
    std::atomic<bool> is_running_{true};
    std::atomic<bool> is_accepting_{true};
};

} // namespace server
