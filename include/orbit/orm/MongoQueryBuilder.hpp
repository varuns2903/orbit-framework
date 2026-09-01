#pragma once
#include <string>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <orbit/database/MongoClient.hpp>

namespace orm {

/**
 * @brief An awaiter for MongoDB queries that maps JSON responses back to C++ ModelType vectors.
 */
template <typename ModelType>
struct MongoGetAwaiter {
    database::MongoClient::QueryAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }
    
    std::vector<ModelType> await_resume() {
        database::MongoClient::QueryResult result = native_awaiter.await_resume();
        std::vector<ModelType> models;
        for (const auto& doc_str : result.documents) {
            try {
                auto j = nlohmann::json::parse(doc_str);
                models.push_back(j.get<ModelType>());
            } catch (...) {
                // Ignore parse errors for now
            }
        }
        return models;
    }
};

/**
 * @brief An awaiter for MongoDB inserts.
 */
template <typename ModelType>
struct MongoInsertAwaiter {
    database::MongoClient::InsertAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }
    
    bool await_resume() {
        return native_awaiter.await_resume();
    }
};

/**
 * @brief A lightweight ORM builder for MongoDB using nlohmann::json.
 */
template <typename ModelType>
class MongoQueryBuilder {
public:
    MongoQueryBuilder(std::shared_ptr<database::MongoClient> db) 
        : db_(std::move(db)) {}
        
    /**
     * @brief Adds a filter using a direct JSON object.
     * @example query.where({{"age", {{"$gt", 18}}}})
     */
    MongoQueryBuilder& where(const nlohmann::json& filter) {
        // Merge filters
        for (auto& el : filter.items()) {
            filter_[el.key()] = el.value();
        }
        return *this;
    }
    
    /**
     * @brief Executes a find query asynchronously and maps results to a vector of ModelType.
     */
    MongoGetAwaiter<ModelType> get_async() {
        return MongoGetAwaiter<ModelType>{ db_->find_async(filter_.dump()) };
    }

    /**
     * @brief Executes an INSERT query for a model.
     */
    MongoInsertAwaiter<ModelType> insert_async(const ModelType& model) {
        nlohmann::json j = model;
        return MongoInsertAwaiter<ModelType>{ db_->insert_async(j.dump()) };
    }

private:
    std::shared_ptr<database::MongoClient> db_;
    nlohmann::json filter_ = nlohmann::json::object();
};

} // namespace orm
