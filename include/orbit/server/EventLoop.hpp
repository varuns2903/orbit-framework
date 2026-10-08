#pragma once
#include <chrono>
#include <functional>
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
    /// @param thread_pool Runs request handlers; shared by every loop of an App
    ///        and must outlive this loop's connections.
    EventLoop(Listener& listener, const routing::Router& router, const config::ServerConfig& config,
              concurrency::ThreadPool& thread_pool, network::TlsContext* tls_context = nullptr,
              network::UdpSocket* quic_socket = nullptr, QuicConnectionManager* quic_manager = nullptr);
    
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
     * @brief Graceful shutdown; safe to call from any thread. Stops accepting,
     *        closes idle connections, lets in-flight requests finish (their
     *        responses carry Connection: close; HTTP/2 gets GOAWAY, WebSocket
     *        close 1001), and force-closes whatever is left at `deadline`.
     *        run() returns once no connection remains.
     */
    void request_shutdown(std::chrono::steady_clock::time_point deadline);

    /**
     * @brief Registers a function the loop calls on every iteration, on the
     *        loop thread. The loop wakes at least every 200 ms, so the hook
     *        also runs when no I/O happens.
     */
    void set_tick_hook(std::function<void()> hook) { tick_hook_ = std::move(hook); }

    /**
     * @brief Gets the thread pool.
     * @return Reference to the thread pool.
     */
    concurrency::ThreadPool& get_thread_pool() { return thread_pool_; }

    /// Makes max_connections one budget shared by the loops of an App: `open`
    /// counts their connections, and a loop reserves a slot in it before each
    /// accept, so concurrent loops cannot together exceed the limit. Call
    /// before run(); `open` must outlive the loop.
    void share_connection_limit(std::atomic<size_t>* open);

    /// Connections this loop currently holds.
    size_t connection_count() const { return connection_manager_.get_connection_count(); }

private:
    void do_accept();
    void do_read_quic();
    // Hands an accepted socket to the connection manager.
    void on_accepted(network::socket_t client_fd, const sockaddr_in& addr);
    // Accepts whatever else is already queued on the listener (POSIX).
    void accept_pending();
    void wait_and_accept(); // accept with a reserved slot (shared limit)
    bool reserve_slot();
    // Stops arming accept until `until` (and while at max_connections).
    void pause_accepting(std::chrono::steady_clock::time_point until);
    void resume_accepting_if_ready();
    bool at_connection_limit() const;

    Listener& listener_;
    TimerManager timer_manager_;
    std::unique_ptr<network::Proactor> proactor_;
    concurrency::ThreadPool& thread_pool_;
    ConnectionManager connection_manager_;
    network::UdpSocket* quic_socket_;
    QuicConnectionManager* quic_manager_;
    
    
    std::atomic<bool> is_running_{true};
    std::function<void()> tick_hook_;
    std::atomic<bool> is_accepting_{true};

    // Graceful shutdown: requested from any thread, carried out by run().
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<std::chrono::steady_clock::rep> shutdown_deadline_{0};
    bool draining_ = false; // loop thread only

    std::chrono::seconds websocket_ping_interval_{0};
    std::chrono::steady_clock::time_point last_websocket_ping_{};
    void drain_step();

    size_t max_connections_ = 0;

    std::atomic<size_t>* shared_open_ = nullptr; // see share_connection_limit()
    bool nonblocking_accepts_ = true; // accepted sockets must match the proactor's own accept
    // Loop-thread only: accept is not armed while paused (fd limit or
    // max_connections reached); new connections wait in the backlog.
    bool accept_paused_ = false;
    std::chrono::steady_clock::time_point accept_resume_at_{};
    std::chrono::steady_clock::time_point last_limit_warning_{};
};

} // namespace server
