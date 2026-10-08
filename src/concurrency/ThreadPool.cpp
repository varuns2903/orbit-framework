#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/utils/Logger.hpp>
#include <exception>

namespace orbit::concurrency {

ThreadPool::ThreadPool(size_t num_threads) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] {
            worker_loop();
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        // Set the flag under the queue mutex. Otherwise a worker that has
        // just checked the wait predicate (stop_ == false, queue empty) but
        // not yet blocked misses the notification, sleeps forever, and
        // join() below never returns.
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all(); // Wake up all threads so they can exit gracefully
    
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push(std::move(task));
    }
    // Wake up exactly one sleeping worker thread to handle the new task
    condition_.notify_one();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        
        {
            // Acquire the lock to check the queue safely
            std::unique_lock<std::mutex> lock(queue_mutex_);
            
            // Go to sleep! The OS will wake this thread up when notify_one() or notify_all() is called,
            // AND the lambda condition returns true.
            condition_.wait(lock, [this] {
                return stop_ || !tasks_.empty();
            });
            
            // If we are shutting down and the queue is empty, exit the thread!
            if (stop_ && tasks_.empty()) {
                return;
            }
            
            // Pop the task from the queue
            task = std::move(tasks_.front());
            tasks_.pop();
        } // The lock is released here!
        
        // Execute the task OUTSIDE the lock. This is critical for parallel performance,
        // otherwise only one thread could execute a task at a time!
        // An exception escaping a worker thread would call std::terminate and
        // take the whole server down, so contain it here.
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("Unhandled exception in worker task: " << e.what());
        } catch (...) {
            LOG_ERROR("Unhandled non-standard exception in worker task");
        }
    }
}

} // namespace concurrency
