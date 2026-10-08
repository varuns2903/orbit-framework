#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <memory>
#include <functional>
#include <optional>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <libpq-fe.h>
#include <orbit/network/Proactor.hpp>
#include <orbit/database/ResultSet.hpp>

namespace orbit::database {

/**
 * @brief An asynchronous PostgreSQL client using libpq and a network Proactor.
 */
class PostgresClient : public std::enable_shared_from_this<PostgresClient> {
public:
    /**
     * @brief Constructs a new PostgresClient.
     * 
     * @param proactor The network Proactor to use for async events.
     * @param conninfo The connection string for the PostgreSQL database.
     */
    PostgresClient(network::Proactor* proactor, const std::string& conninfo);
    ~PostgresClient();

    /**
     * @brief Connects to the PostgreSQL database asynchronously. Called on
     *        a connected client, it drops that connection first (as reconnect()).
     * 
     * @param callback A callback invoked with a boolean indicating success or failure.
     */
    void connect(std::function<void(bool success)> callback);

    /**
     * @brief Executes a query asynchronously.
     * 
     * @param sql The SQL query string.
     * @param callback A callback invoked with the query result upon completion.
     */
    void query(const std::string& sql, std::function<void(const ResultSet& res)> callback);

    /**
     * @brief Executes a statement with $1, $2, ... placeholders bound to
     *        @p params (text format; std::nullopt binds SQL NULL). Values are
     *        sent separately from the SQL text, so they cannot change the
     *        statement.
     */
    void query(const std::string& sql, const std::vector<std::optional<std::string>>& params,
               std::function<void(const ResultSet& res)> callback);

    /**
     * @brief Like the parameterized query(), but prepares @p sql once per
     *        connection and reuses the plan on later calls with the same SQL.
     *
     * Up to max_prepared_statements() statements are cached per connection;
     * beyond that, statements run unprepared. The cache is cleared when the
     * connection is re-established.
     */
    void execute(const std::string& sql, const std::vector<std::optional<std::string>>& params,
                 std::function<void(const ResultSet& res)> callback);

    /// Starts a transaction (BEGIN).
    void begin(std::function<void(const ResultSet& res)> callback);
    /// Commits the current transaction (COMMIT). A transaction that hit an
    /// error is rolled back by the server instead, and the result is a failure.
    void commit(std::function<void(const ResultSet& res)> callback);
    /// Rolls back the current transaction (ROLLBACK).
    void rollback(std::function<void(const ResultSet& res)> callback);
    /// True inside a transaction block (including one that has failed).
    bool in_transaction() const;

    /**
     * @brief Server-side limit on each statement's run time; zero = none.
     *
     * Applied when the connection is established (call it before connect()
     * or reconnect()). A statement over the limit fails with "canceling
     * statement due to statement timeout". For an unreachable server, add
     * libpq's connect_timeout / keepalives / tcp_user_timeout to conninfo.
     */
    void set_statement_timeout(std::chrono::milliseconds timeout) { statement_timeout_ = timeout; }

    /// True if connected and libpq reports the connection usable.
    bool is_healthy() const;
    /// Drops the current connection (if any) and connects again.
    void reconnect(std::function<void(bool success)> callback);

    void set_max_prepared_statements(size_t n) { max_prepared_ = n; }
    size_t max_prepared_statements() const { return max_prepared_; }
    size_t prepared_statement_count() const { return prepared_.size(); }

private:
    void handle_connect(std::function<void(bool)> callback);
    void handle_query(std::function<void(const ResultSet&)> callback);
    void finish_connect(std::function<void(bool)> callback);
    void close_connection();

    network::Proactor* proactor_;
    std::string conninfo_;
    PGconn* conn_{nullptr};
    bool connected_{false};
    std::chrono::milliseconds statement_timeout_{0};
    std::unordered_map<std::string, std::string> prepared_; // SQL -> statement name
    size_t next_statement_ = 0;
    size_t max_prepared_ = 256;
};

} // namespace database
