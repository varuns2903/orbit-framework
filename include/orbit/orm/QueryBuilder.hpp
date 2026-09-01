#pragma once
#include <string>
#include <vector>
#include <memory>
#include <coroutine>
#include <nlohmann/json.hpp>
#include <orbit/database/ResultSet.hpp>

namespace orm {

/**
 * @brief Represents a logical SQL expression (e.g., "age > 18 AND is_active = 'true'").
 */
struct Expr {
    std::string sql;

    Expr operator&&(const Expr& other) const {
        return Expr{"(" + sql + " AND " + other.sql + ")"};
    }
    
    Expr operator||(const Expr& other) const {
        return Expr{"(" + sql + " OR " + other.sql + ")"};
    }
};

/**
 * @brief Represents a database column, allowing C++ operator overloading to build SQL expressions seamlessly.
 */
class Col {
public:
    explicit Col(std::string name) : name_(std::move(name)) {}

    template <typename T>
    Expr operator==(const T& val) const { return Expr{name_ + " = '" + format_val(val) + "'"}; }
    
    template <typename T>
    Expr operator!=(const T& val) const { return Expr{name_ + " != '" + format_val(val) + "'"}; }
    
    template <typename T>
    Expr operator>(const T& val) const { return Expr{name_ + " > '" + format_val(val) + "'"}; }
    
    template <typename T>
    Expr operator<(const T& val) const { return Expr{name_ + " < '" + format_val(val) + "'"}; }
    
    template <typename T>
    Expr operator>=(const T& val) const { return Expr{name_ + " >= '" + format_val(val) + "'"}; }
    
    template <typename T>
    Expr operator<=(const T& val) const { return Expr{name_ + " <= '" + format_val(val) + "'"}; }

private:
    std::string name_;

    template <typename T>
    std::string format_val(const T& val) const {
        if constexpr (std::is_same_v<T, bool>) {
            return val ? "true" : "false";
        } else if constexpr (std::is_arithmetic_v<T>) {
            return std::to_string(val);
        } else {
            return std::string(val); // Strings
        }
    }
};

/**
 * @brief Helper to unify query_async calls for both MysqlClient and PostgresClient.
 */
template <typename DBClient>
auto do_query_async(std::shared_ptr<DBClient> client, const std::string& sql) {
    // We use if constexpr to detect if DBClient has a query_async member.
    // If it does (e.g. MysqlClient), we use it. If not, we assume a free function exists (PostgresClient).
    if constexpr (requires { client->query_async(sql); }) {
        return client->query_async(sql);
    } else {
        return database::query_async(client, sql);
    }
}

template <typename DBClient, typename ModelType>
struct QueryGetAwaiter {
    using NativeAwaiter = decltype(do_query_async(std::declval<std::shared_ptr<DBClient>>(), std::declval<std::string>()));
    NativeAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }
    
    std::vector<ModelType> await_resume() {
        database::ResultSet rs = native_awaiter.await_resume();
        return rs.to_json().get<std::vector<ModelType>>();
    }
};

template <typename DBClient, typename ModelType>
struct QueryInsertAwaiter {
    using NativeAwaiter = decltype(do_query_async(std::declval<std::shared_ptr<DBClient>>(), std::declval<std::string>()));
    NativeAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }
    
    uint64_t await_resume() {
        database::ResultSet rs = native_awaiter.await_resume();
        return rs.affected_rows();
    }
};

/**
 * @brief A lightweight, JSON-backed ORM Query Builder.
 */
template <typename DBClient, typename ModelType>
class QueryBuilder {
public:
    QueryBuilder(std::shared_ptr<DBClient> db, const std::string& table) 
        : db_(std::move(db)), table_(table) {}
        
    /**
     * @brief Adds a WHERE clause using an explicit field, operator, and value.
     */
    QueryBuilder& where(const std::string& field, const std::string& op, const std::string& val) {
        wheres_.push_back(field + " " + op + " '" + val + "'");
        return *this;
    }

    /**
     * @brief Adds a WHERE clause using C++ Expression Templates for a clean DSL.
     * 
     * @example query_User(db).where(orm::Col("age") >= 18 && orm::Col("is_active") == true)
     */
    QueryBuilder& where(const Expr& expr) {
        wheres_.push_back(expr.sql);
        return *this;
    }
    
    /**
     * @brief Executes a SELECT query asynchronously and maps results to a vector of ModelType.
     */
    QueryGetAwaiter<DBClient, ModelType> get_async() {
        std::string sql = "SELECT * FROM " + table_;
        if (!wheres_.empty()) {
            sql += " WHERE ";
            for (size_t i = 0; i < wheres_.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += wheres_[i];
            }
        }
        return QueryGetAwaiter<DBClient, ModelType>{ do_query_async(db_, sql) };
    }

    /**
     * @brief Executes an INSERT query for a model.
     */
    QueryInsertAwaiter<DBClient, ModelType> insert_async(const ModelType& model) {
        nlohmann::json j = model;
        std::string cols = "";
        std::string vals = "";
        
        bool first = true;
        for (auto& el : j.items()) {
            if (!first) { cols += ", "; vals += ", "; }
            cols += el.key();
            
            if (el.value().is_string()) {
                vals += "'" + el.value().get<std::string>() + "'";
            } else if (el.value().is_null()) {
                vals += "NULL";
            } else {
                vals += el.value().dump(); // dump converts numbers/booleans to string representations
            }
            first = false;
        }

        std::string sql = "INSERT INTO " + table_ + " (" + cols + ") VALUES (" + vals + ");";
        return QueryInsertAwaiter<DBClient, ModelType>{ do_query_async(db_, sql) };
    }

private:
    std::shared_ptr<DBClient> db_;
    std::string table_;
    std::vector<std::string> wheres_;
};

} // namespace orm
