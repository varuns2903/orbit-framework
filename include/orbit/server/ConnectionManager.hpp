#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/server/Connection.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/server/TimerManager.hpp>
#include <orbit/network/TlsContext.hpp>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>

namespace orbit::server {

/**
 * @brief Manages active HTTP connections.
 */
class ConnectionManager {
public:
    /**
     * @brief Constructs a ConnectionManager.
     * @param proactor The proactor for I/O.
     * @param router The router.
     * @param thread_pool The thread pool.
     * @param timer_manager The timer manager.
     * @param max_body_size The maximum body size allowed.
     * @param tls_context The TLS context (optional).
     */
    ConnectionManager(network::Proactor& proactor, const routing::Router& router, concurrency::ThreadPool& thread_pool, TimerManager& timer_manager, size_t max_body_size, network::TlsContext* tls_context = nullptr);
    /// Connections still open (the server is going away) are closed for
    /// anyone still holding one as a ResponseWriter.
    ~ConnectionManager();

    /**
     * @brief Sets the timeouts applied to connections accepted from now on.
     */
    void set_timeouts(const ConnectionTimeouts& timeouts) { timeouts_ = timeouts; }
    void set_limits(const ConnectionLimits& limits) { limits_ = limits; }

    /// Called once for each connection removed (from the thread removing it).
    void set_on_removed(std::function<void()> callback) { on_removed_ = std::move(callback); }

    /// Event-loop thread: pings every WebSocket connection.
    void ping_websockets();

    /**
     * @brief Adds a new connection to the manager.
     * @param socket The connection socket.
     * @param client_ip The client's IP address.
     */
    void add_connection(network::Socket socket, const std::string& client_ip);
    
    /**
     * @brief Removes a connection by file descriptor.
     * @param fd The file descriptor.
     */
    void remove_connection(int fd);
    
    /**
     * @brief Gets the current number of active connections.
     * @return The number of connections.
     */
    size_t get_connection_count() const;

    /// Event-loop thread: asks every connection to finish up (see
    /// Connection::on_server_shutdown). Called repeatedly while draining.
    void notify_shutdown();

    /// Any thread: set as soon as shutdown is requested, before the event
    /// loop gets to notify_shutdown(), so responses sent in between already
    /// carry "Connection: close".
    void mark_shutting_down() { shutting_down_ = true; }
    bool shutting_down() const { return shutting_down_.load(); }

private:
    std::atomic<bool> shutting_down_{false};
    network::Proactor& proactor_;
    const routing::Router& router_;
    concurrency::ThreadPool& thread_pool_;
    TimerManager& timer_manager_;
    ConnectionTimeouts timeouts_;
    ConnectionLimits limits_;
    
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    mutable std::mutex map_mutex_;
    std::function<void()> on_removed_;
    size_t max_body_size_;
    network::TlsContext* tls_context_;
};

} // namespace server
