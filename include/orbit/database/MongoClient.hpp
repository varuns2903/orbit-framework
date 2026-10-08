#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <memory>
#include <coroutine>
#include <vector>
#include <stdexcept>
#include <atomic>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <orbit/concurrency/ThreadPool.hpp>

namespace orbit::database {

/**
 * @brief An asynchronous MongoDB client using coroutines.
 */
class MongoClient {
public:
    /**
     * @brief Configuration options for the MongoDB client.
     */
    struct Config {
        std::string uri{"mongodb://localhost:27017"};
        std::string dbname;
        std::string collection_name;
    };

    struct QueryResult {
        std::vector<std::string> documents;
    };

    /**
     * @brief Constructs a new MongoClient.
     * 
     * @param thread_pool The thread pool to use for asynchronous operations.
     * @param config The configuration settings for the MongoDB client.
     */
    MongoClient(concurrency::ThreadPool& thread_pool, const Config& config);
    ~MongoClient();

    MongoClient(const MongoClient&) = delete;
    MongoClient& operator=(const MongoClient&) = delete;

    struct QueryAwaiter {
        MongoClient& client;
        std::string query_json;
        QueryResult result;
        std::string error_msg;
        std::coroutine_handle<> coro;

        bool await_ready() const { return false; }
        void await_suspend(std::coroutine_handle<> h);
        QueryResult await_resume();
    };
    
    struct InsertAwaiter {
        MongoClient& client;
        std::string doc_json;
        bool success{false};
        std::string error_msg;
        std::coroutine_handle<> coro;

        bool await_ready() const { return false; }
        void await_suspend(std::coroutine_handle<> h);
        bool await_resume();
    };

    /**
     * @brief Executes a find query asynchronously.
     * 
     * @param query_json The JSON string representing the query.
     * @return A QueryAwaiter that can be co_awaited for the result.
     * 
     * @code
     * auto result = co_await client.find_async("{\"name\": \"test\"}");
     * @endcode
     */
    QueryAwaiter find_async(std::string query_json);

    /**
     * @brief Inserts a document asynchronously.
     * 
     * @param doc_json The JSON string representing the document to insert.
     * @return An InsertAwaiter that can be co_awaited for success status.
     */
    InsertAwaiter insert_async(std::string doc_json);

    /**
     * @brief Gets the associated thread pool.
     * @return Reference to the thread pool.
     */
    concurrency::ThreadPool& get_thread_pool() { return thread_pool_; }

    /**
     * @brief Gets the underlying MongoDB client pool.
     * @return Pointer to the mongoc_client_pool_t instance.
     */
    mongoc_client_pool_t* get_pool() { return pool_; }

    /**
     * @brief Gets the configuration used by this client.
     * @return Constant reference to the configuration.
     */
    const Config& get_config() const { return config_; }

private:
    concurrency::ThreadPool& thread_pool_;
    Config config_;
    mongoc_uri_t* uri_{nullptr};
    mongoc_client_pool_t* pool_{nullptr};
    
};

} // namespace database
