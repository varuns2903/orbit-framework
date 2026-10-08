#include <orbit/network/ConnectionPool.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <stdexcept>
#include <iostream>
#include <orbit/network/PlatformSocket.hpp>
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#include <openssl/ssl.h>

namespace orbit::network {

std::pair<int, void*> ConnectionPool::acquire(const std::string& host, int port) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::string key = host + ":" + std::to_string(port);
    
    auto it = pool_.find(key);
    if (it != pool_.end() && !it->second.empty()) {
        int fd = it->second.back().fd;
        void* ssl = it->second.back().ssl;
        it->second.pop_back();
        
        // Simple check if socket is still alive by peeking 1 byte without blocking
        char buf;
        ssize_t ret = recv(fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (ret == 0) {
            // Socket was closed by peer cleanly
            if (ssl) SSL_free(static_cast<SSL*>(ssl));
            network::close_socket(fd);
            // Recursive fallback to get the next one
            lock.unlock();
            return acquire(host, port);
        } else if ((ret > 0 && !ssl) || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Bytes waiting on an idle plain connection (typically a 408 sent
            // before the server closes it) would be read as the next
            // request's response, so like an error it cannot be reused. Not
            // for TLS, where they may be harmless post-handshake records such
            // as session tickets.
            if (ssl) SSL_free(static_cast<SSL*>(ssl));
            network::close_socket(fd);
            lock.unlock();
            return acquire(host, port);
        }
        
        return {fd, ssl};
    }
    
    return {-1, nullptr};
}

void ConnectionPool::release(const std::string& host, int port, int fd, void* ssl) {
    if (fd < 0) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = host + ":" + std::to_string(port);
    
    PooledConnection conn;
    conn.fd = fd;
    conn.ssl = ssl;
    conn.last_used = std::chrono::steady_clock::now();
    
    pool_[key].push_back(conn);
}

ConnectionPool::~ConnectionPool() {
    // Pooled connections own their socket and TLS state; release them rather
    // than dropping the raw handles with the map.
    for (auto& [key, conns] : pool_) {
        for (auto& conn : conns) {
            if (conn.ssl) SSL_free(static_cast<SSL*>(conn.ssl));
            network::close_socket(conn.fd);
        }
    }
}

void ConnectionPool::cleanup_stale_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [key, conns] : pool_) {
        auto it = conns.begin();
        while (it != conns.end()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->last_used).count() > 60) {
                if (it->ssl) SSL_free(static_cast<SSL*>(it->ssl));
                network::close_socket(it->fd);
                it = conns.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace network
