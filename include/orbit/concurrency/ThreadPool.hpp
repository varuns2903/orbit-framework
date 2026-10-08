#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace orbit::concurrency {

class ThreadPool {
public:
    // Create a thread pool with a specific number of threads
    explicit ThreadPool(size_t num_threads);
    
    // Join all threads and shut down the pool safely
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Add a new task to the queue for a worker thread to pick up
    void enqueue(std::function<void()> task);

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    
    // Synchronization primitives
    std::mutex queue_mutex_;
    std::condition_variable condition_;
    
    // Atomic flag to signal threads to stop processing and exit
    std::atomic<bool> stop_{false};
};

} // namespace concurrency
