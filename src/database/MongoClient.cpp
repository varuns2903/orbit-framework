#include <orbit/database/MongoClient.hpp>
#include <iostream>
#include <mutex>

namespace database {

std::atomic<int> MongoClient::init_count_{0};

MongoClient::MongoClient(concurrency::ThreadPool& thread_pool, const Config& config)
    : thread_pool_(thread_pool), config_(config) {
    if (init_count_.fetch_add(1) == 0) {
        mongoc_init();
    }

    bson_error_t error;
    uri_ = mongoc_uri_new_with_error(config_.uri.c_str(), &error);
    if (!uri_) {
        throw std::runtime_error(std::string("Failed to parse MongoDB URI: ") + error.message);
    }

    pool_ = mongoc_client_pool_new(uri_);
    if (!pool_) {
        throw std::runtime_error("Failed to create MongoDB client pool");
    }
    
    // Set an application name
    mongoc_client_pool_set_appname(pool_, "orbit-framework");
}

MongoClient::~MongoClient() {
    if (pool_) {
        mongoc_client_pool_destroy(pool_);
    }
    if (uri_) {
        mongoc_uri_destroy(uri_);
    }
    
    if (init_count_.fetch_sub(1) == 1) {
        mongoc_cleanup();
    }
}

// QueryAwaiter
void MongoClient::QueryAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro = h;
    client.get_thread_pool().enqueue([this]() {
        mongoc_client_t* mclient = mongoc_client_pool_pop(client.get_pool());
        mongoc_collection_t* collection = mongoc_client_get_collection(mclient, client.get_config().dbname.c_str(), client.get_config().collection_name.c_str());
        
        bson_error_t error;
        bson_t* query = bson_new_from_json(reinterpret_cast<const uint8_t*>(query_json.c_str()), query_json.length(), &error);
        
        if (!query) {
            error_msg = std::string("BSON Parse Error: ") + error.message;
            mongoc_collection_destroy(collection);
            mongoc_client_pool_push(client.get_pool(), mclient);
            coro.resume();
            return;
        }

        mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
        
        const bson_t* doc;
        while (mongoc_cursor_next(cursor, &doc)) {
            char* str = bson_as_canonical_extended_json(doc, nullptr);
            if (str) {
                result.documents.push_back(std::string(str));
                bson_free(str);
            }
        }
        
        if (mongoc_cursor_error(cursor, &error)) {
            error_msg = std::string("MongoDB Cursor Error: ") + error.message;
        }

        mongoc_cursor_destroy(cursor);
        bson_destroy(query);
        mongoc_collection_destroy(collection);
        mongoc_client_pool_push(client.get_pool(), mclient);
        
        coro.resume();
    });
}

MongoClient::QueryResult MongoClient::QueryAwaiter::await_resume() {
    if (!error_msg.empty()) {
        throw std::runtime_error(error_msg);
    }
    return result;
}

MongoClient::QueryAwaiter MongoClient::find_async(std::string query_json) {
    return QueryAwaiter{*this, std::move(query_json), {}, "", nullptr};
}

// InsertAwaiter
void MongoClient::InsertAwaiter::await_suspend(std::coroutine_handle<> h) {
    coro = h;
    client.get_thread_pool().enqueue([this]() {
        mongoc_client_t* mclient = mongoc_client_pool_pop(client.get_pool());
        mongoc_collection_t* collection = mongoc_client_get_collection(mclient, client.get_config().dbname.c_str(), client.get_config().collection_name.c_str());
        
        bson_error_t error;
        bson_t* doc = bson_new_from_json(reinterpret_cast<const uint8_t*>(doc_json.c_str()), static_cast<ssize_t>(doc_json.length()), &error);
        
        if (!doc) {
            error_msg = std::string("BSON Parse Error: ") + error.message;
            mongoc_collection_destroy(collection);
            mongoc_client_pool_push(client.get_pool(), mclient);
            coro.resume();
            return;
        }

        success = mongoc_collection_insert_one(collection, doc, nullptr, nullptr, &error);
        
        if (!success) {
            error_msg = std::string("MongoDB Insert Error: ") + error.message;
        }

        bson_destroy(doc);
        mongoc_collection_destroy(collection);
        mongoc_client_pool_push(client.get_pool(), mclient);
        
        coro.resume();
    });
}

bool MongoClient::InsertAwaiter::await_resume() {
    if (!error_msg.empty()) {
        throw std::runtime_error(error_msg);
    }
    return success;
}

MongoClient::InsertAwaiter MongoClient::insert_async(std::string doc_json) {
    return InsertAwaiter{*this, std::move(doc_json), false, "", nullptr};
}

} // namespace database
