#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>
#include <orbit/network/Proactor.hpp>

namespace orbit::database {

/**
 * @brief Health checking and reconnecting for a ConnectionPool.
 *
 * With a health check, a client found unusable (when acquired or released)
 * is dropped and a replacement is connected in the background; failed
 * attempts are retried with exponential backoff, from initial_backoff
 * doubling up to max_backoff.
 */
template <typename ClientType>
struct PoolOptions {
    /// True if a client is usable, e.g. `[](auto& c) { return c.is_healthy(); }`.
    /// Empty: clients are never checked or replaced.
    std::function<bool(ClientType&)> health_check;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{30000};
    /// Runs a task after a delay (the backoff). The default sleeps on a
    /// detached thread; pass one backed by your event loop's timers instead.
    std::function<void(std::chrono::milliseconds, std::function<void()>)> schedule;
};

/**
 * @brief A thread-safe connection pool for managing reusable database client connections.
 *
 * @tparam ClientType The type of database client to manage.
 */
template <typename ClientType>
class ConnectionPool : public std::enable_shared_from_this<ConnectionPool<ClientType>> {
public:
    using ClientFactory = std::function<std::shared_ptr<ClientType>()>;
    using Connector = std::function<void(std::shared_ptr<ClientType>, std::function<void(bool)>)>;

    /**
     * @brief Constructs a new ConnectionPool.
     *
     * @param max_size The maximum number of connections to maintain in the pool.
     * @param factory A callable that creates new instances of ClientType.
     * @param options Health checking and reconnecting (off by default).
     */
    ConnectionPool(size_t max_size, ClientFactory factory, PoolOptions<ClientType> options = {})
        : max_size_(max_size), factory_(std::move(factory)), options_(std::move(options)) {
        if (!options_.schedule) {
            options_.schedule = [](std::chrono::milliseconds delay, std::function<void()> task) {
                std::thread([delay, task = std::move(task)] {
                    std::this_thread::sleep_for(delay);
                    task();
                }).detach();
            };
        }
    }

    /**
     * @brief Initializes the pool by establishing the initial set of connections.
     *
     * @param connector A function used to connect a client asynchronously.
     * @param on_ready Callback invoked when the pool is initialized. The parameter is true if initialization was successful.
     *        With a health check, connections that failed are retried in the background.
     */
    void init(Connector connector, std::function<void(bool success)> on_ready) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connector_ = connector;
        }
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
                bool all_done = false;
                bool all_ok = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    if (success) {
                        (*success_count)++;
                    } else {
                        (*fail_count)++;
                    }
                    all_done = *success_count + *fail_count == self->max_size_;
                    all_ok = *fail_count == 0;
                }
                if (success) {
                    self->release(client);
                } else if (self->options_.health_check) {
                    self->replace(self->options_.initial_backoff);
                }
                if (all_done) on_ready(all_ok);
            });
        }
    }

    /**
     * @brief Acquires a database connection asynchronously from the pool.
     *
     * With a health check, an idle client that is no longer usable is
     * dropped (and replaced in the background) rather than handed out.
     *
     * @param callback Callback invoked with a shared pointer to a ready client when available.
     */
    void acquire(std::function<void(std::shared_ptr<ClientType>)> callback) {
        std::shared_ptr<ClientType> client;
        size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (!idle_connections_.empty()) {
                auto candidate = idle_connections_.front();
                idle_connections_.pop();
                if (!options_.health_check || options_.health_check(*candidate)) {
                    client = std::move(candidate);
                    break;
                }
                ++dropped;
            }
            if (!client) wait_queue_.push(std::move(callback));
        }
        for (size_t i = 0; i < dropped; ++i) replace(std::chrono::milliseconds(0));
        // Invoke callback outside the lock to prevent deadlocks
        if (client) callback(client);
    }

    /**
     * @brief Releases a connection back to the pool.
     *
     * With a health check, a client that is no longer usable is dropped and
     * a replacement is connected in the background.
     *
     * @param client The client to return to the pool.
     */
    void release(std::shared_ptr<ClientType> client) {
        if (options_.health_check && !options_.health_check(*client)) {
            replace(std::chrono::milliseconds(0));
            return;
        }
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

    /// Idle clients ready to be acquired.
    size_t idle_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_connections_.size();
    }

    /// Acquirers waiting for a client.
    size_t waiting_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return wait_queue_.size();
    }

    /// Replacements being connected (or waiting out a backoff).
    size_t reconnecting_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return reconnecting_;
    }

private:
    size_t max_size_;
    ClientFactory factory_;
    PoolOptions<ClientType> options_;

    std::mutex mutex_;
    Connector connector_;
    size_t reconnecting_ = 0;
    std::queue<std::shared_ptr<ClientType>> idle_connections_;
    std::queue<std::function<void(std::shared_ptr<ClientType>)>> wait_queue_;

    // Connects a new client in place of a dropped or failed one, after
    // @p delay; a failed attempt is retried with twice the delay (capped).
    void replace(std::chrono::milliseconds delay) {
        Connector connector;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!connector_) return; // init() was never called: nothing to connect with
            connector = connector_;
            ++reconnecting_;
        }
        std::weak_ptr<ConnectionPool> weak = this->shared_from_this();
        auto attempt = [weak, connector, delay]() {
            auto self = weak.lock();
            if (!self) return; // the pool is gone
            auto client = self->factory_();
            connector(client, [weak, client, delay](bool success) {
                auto self = weak.lock();
                if (!self) return;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    --self->reconnecting_;
                }
                if (success) {
                    self->release(client);
                } else {
                    auto next = std::max(self->options_.initial_backoff, delay * 2);
                    self->replace(std::min(next, self->options_.max_backoff));
                }
            });
        };
        if (delay.count() == 0) {
            attempt();
        } else {
            options_.schedule(delay, attempt);
        }
    }
};

} // namespace database
