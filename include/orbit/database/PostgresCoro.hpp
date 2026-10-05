#pragma once
#include <orbit/database/PostgresClient.hpp>
#include <coroutine>
#include <memory>
#include <string>
#include <optional>
#include <vector>

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
    std::vector<std::optional<std::string>> params;
    bool parameterized = false;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto done = [this, h](const ResultSet& r) {
            result = r;
            h.resume();
        };
        if (parameterized) {
            client->query(sql, params, done);
        } else {
            client->query(sql, done);
        }
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
    return QueryAwaiter{std::move(client), sql, ResultSet{}, {}, false};
}

/**
 * @brief Runs a statement with $1, $2, ... placeholders bound to @p params.
 *
 * @code
 * co_await query_async(db, "SELECT * FROM users WHERE name = $1", {name});
 * @endcode
 */
inline QueryAwaiter query_async(std::shared_ptr<PostgresClient> client, const std::string& sql,
                                std::vector<std::optional<std::string>> params) {
    return QueryAwaiter{std::move(client), sql, ResultSet{}, std::move(params), true};
}

/**
 * @brief Awaits any PostgresClient operation that reports a ResultSet.
 */
struct ResultAwaiter {
    std::function<void(std::function<void(const ResultSet&)>)> start;
    ResultSet result;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        start([this, h](const ResultSet& r) {
            result = r;
            h.resume();
        });
    }
    ResultSet await_resume() { return std::move(result); }
};

/// Runs a statement through the prepared-statement cache (PostgresClient::execute).
inline ResultAwaiter execute_async(std::shared_ptr<PostgresClient> client, std::string sql,
                                   std::vector<std::optional<std::string>> params = {}) {
    return ResultAwaiter{[client = std::move(client), sql = std::move(sql), params = std::move(params)](auto done) {
        client->execute(sql, params, std::move(done));
    }, ResultSet{}};
}

/**
 * @brief Transaction control for coroutines.
 *
 * @code
 * if (!(co_await begin_async(db)).ok()) co_return;
 * auto debit = co_await execute_async(db, "UPDATE accounts SET balance = balance - $1 WHERE id = $2", {amount, from});
 * auto credit = co_await execute_async(db, "UPDATE accounts SET balance = balance + $1 WHERE id = $2", {amount, to});
 * if (debit.ok() && credit.ok()) co_await commit_async(db);
 * else co_await rollback_async(db);
 * @endcode
 */
inline ResultAwaiter begin_async(std::shared_ptr<PostgresClient> client) {
    return ResultAwaiter{[client = std::move(client)](auto done) { client->begin(std::move(done)); }, ResultSet{}};
}

inline ResultAwaiter commit_async(std::shared_ptr<PostgresClient> client) {
    return ResultAwaiter{[client = std::move(client)](auto done) { client->commit(std::move(done)); }, ResultSet{}};
}

inline ResultAwaiter rollback_async(std::shared_ptr<PostgresClient> client) {
    return ResultAwaiter{[client = std::move(client)](auto done) { client->rollback(std::move(done)); }, ResultSet{}};
}

} // namespace database
