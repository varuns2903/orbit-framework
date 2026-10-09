#include <vector>
#include <orbit/server/ConnectionManager.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <iostream>

namespace orbit::server {

ConnectionManager::ConnectionManager(network::Proactor& proactor, const routing::Router& router, concurrency::ThreadPool& thread_pool, TimerManager& timer_manager, size_t max_body_size, network::TlsContext* tls_context)
    : proactor_(proactor), router_(router), thread_pool_(thread_pool), timer_manager_(timer_manager), max_body_size_(max_body_size), tls_context_(tls_context) {}

ConnectionManager::~ConnectionManager() {
    std::unordered_map<int, std::shared_ptr<Connection>> remaining;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        remaining.swap(connections_);
    }
    for (auto& [fd, conn] : remaining) conn->on_removed();
}

void ConnectionManager::add_connection(network::Socket socket, const std::string& client_ip) {
    int fd = socket.fd();
    auto connection = std::make_shared<Connection>(
        std::move(socket), client_ip, proactor_, router_, *this, thread_pool_, timer_manager_, max_body_size_, tls_context_
    );
    
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        connections_[fd] = connection;
    }
    
    utils::PrometheusRegistry::get_instance().inc_gauge("orbit_active_connections", "type=\"tcp\"");
    
    connection->set_timeouts(timeouts_);
    connection->set_limits(limits_);

    // With Proactor, we kick off the first read immediately!
    connection->start();
}

void ConnectionManager::remove_connection(int fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = connections_.find(fd);
        if (it != connections_.end()) {
            conn = it->second;
            connections_.erase(it);
            utils::PrometheusRegistry::get_instance().dec_gauge("orbit_active_connections", "type=\"tcp\"");
        }
    }
    
    if (conn) {
        // Stop new I/O first, then cancel what is pending; in the other order
        // a worker thread could register fresh I/O in between.
        conn->on_removed();
        // Tell the peer now. The descriptor is only closed when the last
        // reference to the connection goes; on IOCP a pending receive holds
        // one until its cancellation is reaped, which could leave the client
        // waiting. Everything queued has been written by the time we close.
        network::shutdown_socket(fd);
        // Cancel all pending asynchronous operations in the Proactor
        proactor_.remove(fd);
        if (on_removed_) on_removed_();
        // The shared_ptr will be destroyed here, triggering Connection::~Connection
    }
}

void ConnectionManager::ping_websockets() {
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        snapshot.reserve(connections_.size());
        for (auto& [fd, conn] : connections_) snapshot.push_back(conn);
    }
    for (auto& conn : snapshot) conn->ping_if_websocket();
}

void ConnectionManager::notify_shutdown() {
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        snapshot.reserve(connections_.size());
        for (auto& [fd, conn] : connections_) snapshot.push_back(conn);
    }
    // Outside the lock: closing a connection removes it from the map.
    for (auto& conn : snapshot) conn->on_server_shutdown();
}

size_t ConnectionManager::get_connection_count() const {
    std::lock_guard<std::mutex> lock(map_mutex_);
    return connections_.size();
}

} // namespace server
