#pragma once
#include <mysql.h>
#include <string>
#include <memory>
#include <coroutine>
#include <vector>
#include <stdexcept>
#include <functional>
#include <orbit/network/Proactor.hpp>
#include <orbit/database/ResultSet.hpp>

namespace database {

/**
 * @brief An asynchronous MySQL client using coroutines and a network Proactor.
 */
class MysqlClient {
public:
    /**
     * @brief Configuration options for the MySQL client.
     */
    struct Config {
        std::string host;
        int port{3306};
        std::string user;
        std::string password;
        std::string dbname;
    };



    /**
     * @brief Constructs a new MysqlClient.
     * 
     * @param proactor The network Proactor to use for async events.
     * @param config The configuration settings for the MySQL connection.
     */
    MysqlClient(network::Proactor& proactor, const Config& config);
    ~MysqlClient();

    // Delete copy semantics
    MysqlClient(const MysqlClient&) = delete;
    MysqlClient& operator=(const MysqlClient&) = delete;

    // Awaiter for connecting
    struct ConnectAwaiter {
        MysqlClient& client;
        MYSQL* ret_ptr{nullptr};
        int status{0};
        std::coroutine_handle<> coro;

        bool await_ready() const { return false; }
        void await_suspend(std::coroutine_handle<> h);
        void await_resume();
        void resume_loop();
    };

    // Awaiter for executing a query
    struct QueryAwaiter {
        MysqlClient& client;
        std::string query;
        int err{0};
        int status{0};
        std::coroutine_handle<> coro;
        ResultSet result;

        bool await_ready() const { return false; }
        void await_suspend(std::coroutine_handle<> h);
        ResultSet await_resume();
        void resume_loop();
        void process_result_start();
        void process_result_cont();
        
        MYSQL_RES* res{nullptr};
    };

    /**
     * @brief Connects to the MySQL database asynchronously.
     * 
     * @return A ConnectAwaiter that can be co_awaited.
     */
    ConnectAwaiter connect_async();

    /**
     * @brief Executes a query asynchronously.
     * 
     * @param query The SQL query string.
     * @return A QueryAwaiter that can be co_awaited for the result.
     * 
     * @code
     * auto result = co_await client.query_async("SELECT * FROM users");
     * @endcode
     */
    QueryAwaiter query_async(std::string query);

    /**
     * @brief Closes the MySQL connection.
     */
    void close();

    /**
     * @brief Gets the associated network proactor.
     * @return Reference to the network Proactor.
     */
    network::Proactor& get_proactor() { return proactor_; }

    /**
     * @brief Gets the underlying MySQL connection object.
     * @return Pointer to the MYSQL object.
     */
    MYSQL* get_mysql() { return mysql_; }

private:
    network::Proactor& proactor_;
    Config config_;
    MYSQL* mysql_{nullptr};
    bool connected_{false};

    void wait_for_status(int status, std::function<void()> callback);
};

} // namespace database
