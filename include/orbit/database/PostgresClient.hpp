#pragma once
#include <string>
#include <memory>
#include <functional>
#include <libpq-fe.h>
#include <orbit/network/Proactor.hpp>
#include <orbit/database/ResultSet.hpp>

namespace database {

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
     * @brief Connects to the PostgreSQL database asynchronously.
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

private:
    void handle_connect(std::function<void(bool)> callback);
    void handle_query(std::function<void(const ResultSet&)> callback);

    network::Proactor* proactor_;
    std::string conninfo_;
    PGconn* conn_{nullptr};
    bool connected_{false};
};

} // namespace database
