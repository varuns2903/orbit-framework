#pragma once
#include <vector>
#include <queue>
#include <memory>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <orbit/network/Proactor.hpp>

namespace database {

/**
 * @brief A thread-safe connection pool for managing reusable database client connections.
 * 
 * @tparam ClientType The type of database client to manage.
 */
template <typename ClientType>
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool<ClientType>> {
public:
    using ClientFactory = std::function<std::shared_ptr<ClientType>()>;
    
    /**
     * @brief Constructs a new ConnectionPool.
     * 
     * @param max_size The maximum number of connections to maintain in the pool.
     * @param factory A callable that creates new instances of ClientType.
     */
    ConnectionPool(size_t max_size, ClientFactory factory)
        : max_size_(max_size), factory_(std::move(factory)) {}

    /**
     * @brief Initializes the pool by establishing the initial set of connections.
     * 
     * @param connector A function used to connect a client asynchronously.
     * @param on_ready Callback invoked when the pool is initialized. The parameter is true if initialization was successful.
     */
    void init(std::function<void(std::shared_ptr<ClientType>, std::function<void(bool)>)> connector, std::function<void(bool success)> on_ready) {
        if (max_size_ == 0) {
            on_ready(true);
            return;
        }
        
        auto self = this->shared_from_this();
        auto success_count = std::make_shared<size_t>(0);
        auto fail_count = std::make_shared<size_t>(0);
        
        for (size_t i = 0; i < max_size_; ++i) {
            auto client = factory_();
            connector(client, [self, client, on_ready, success_count, fail_count](bool success) {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (success) {
                    self->idle_connections_.push(client);
                    (*success_count)++;
                } else {
                    (*fail_count)++;
                }
                
                if (*success_count + *fail_count == self->max_size_) {
                    on_ready(*fail_count == 0);
                }
            });
        }
    }

    /**
     * @brief Acquires a database connection asynchronously from the pool.
     * 
     * @param callback Callback invoked with a shared pointer to a ready client when available.
     */
    void acquire(std::function<void(std::shared_ptr<ClientType>)> callback) {
        std::shared_ptr<ClientType> client = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!idle_connections_.empty()) {
                client = idle_connections_.front();
                idle_connections_.pop();
            } else {
                // Queue the request
                wait_queue_.push(std::move(callback));
                return;
            }
        }
        // Invoke callback outside the lock to prevent deadlocks
        if (client) {
            callback(client);
        }
    }

    /**
     * @brief Releases a connection back to the pool.
     * 
     * @param client The client to return to the pool.
     */
    void release(std::shared_ptr<ClientType> client) {
        std::function<void(std::shared_ptr<ClientType>)> next_callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!wait_queue_.empty()) {
                next_callback = std::move(wait_queue_.front());
                wait_queue_.pop();
            } else {
                idle_connections_.push(client);
                return;
            }
        }
        // Dispatch immediately to next waiter outside the lock
        if (next_callback) {
            next_callback(client);
        }
    }

private:
    size_t max_size_;
    ClientFactory factory_;
    
    std::mutex mutex_;
    std::queue<std::shared_ptr<ClientType>> idle_connections_;
    std::queue<std::function<void(std::shared_ptr<ClientType>)>> wait_queue_;
};

} // namespace database
