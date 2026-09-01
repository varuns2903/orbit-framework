#pragma once
#include <orbit/database/PostgresClient.hpp>
#include <coroutine>
#include <memory>
#include <string>

namespace database {

/**
 * @brief A coroutine awaiter for connecting a PostgresClient asynchronously.
 */
struct ConnectAwaiter {
    std::shared_ptr<PostgresClient> client;
    bool success = false;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        client->connect([this, h](bool s) {
            success = s;
            h.resume();
        });
    }
    bool await_resume() { return success; }
};

/**
 * @brief Creates an awaiter for connecting a PostgresClient.
 * 
 * @param client A shared pointer to the PostgresClient.
 * @return A ConnectAwaiter that can be co_awaited.
 */
inline ConnectAwaiter connect_async(std::shared_ptr<PostgresClient> client) {
    return ConnectAwaiter{std::move(client)};
}

/**
 * @brief A coroutine awaiter for executing a query on a PostgresClient asynchronously.
 */
struct QueryAwaiter {
    std::shared_ptr<PostgresClient> client;
    std::string sql;
    ResultSet result;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        client->query(sql, [this, h](const ResultSet& r) {
            result = r;
            h.resume();
        });
    }
    ResultSet await_resume() { return result; }
};

/**
 * @brief Creates an awaiter for executing a query on a PostgresClient.
 * 
 * @param client A shared pointer to the PostgresClient.
 * @param sql The SQL query string to execute.
 * @return A QueryAwaiter that can be co_awaited for the result.
 * 
 * @code
 * auto result = co_await query_async(client, "SELECT * FROM items");
 * @endcode
 */
inline QueryAwaiter query_async(std::shared_ptr<PostgresClient> client, const std::string& sql) {
    return QueryAwaiter{std::move(client), sql, ResultSet{}};
}

} // namespace database
