#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <chrono>

namespace orbit::network {

struct PooledConnection {
    int fd{-1};
    void* ssl{nullptr};
    std::chrono::steady_clock::time_point last_used;
};

class ConnectionPool {
public:
    static ConnectionPool& get_instance() {
        static ConnectionPool instance;
        return instance;
    }

    // Gets a connection immediately if available, returning pair {fd, ssl_ptr}
    std::pair<int, void*> acquire(const std::string& host, int port);
    
    // Returns a connection back to the pool
    void release(const std::string& host, int port, int fd, void* ssl = nullptr);
    
    // Removes closed or stale connections
    void cleanup_stale_connections();

    ~ConnectionPool();

private:
    ConnectionPool() = default;
    
    std::mutex mutex_;
    // Key: "host:port"
    std::unordered_map<std::string, std::vector<PooledConnection>> pool_;
};

} // namespace network
