#pragma once
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <orbit/network/Socket.hpp>

namespace database {

/**
 * @brief A synchronous Redis client providing core operations.
 */
class RedisClient {
public:
    /**
     * @brief Constructs a new RedisClient.
     * 
     * @param host The Redis server hostname or IP address.
     * @param port The Redis server port number.
     */
    RedisClient(const std::string& host, int port);
    ~RedisClient();

    /**
     * @brief Connects to the Redis server.
     * 
     * @return true if the connection was successful, false otherwise.
     */
    bool connect();

    /**
     * @brief Disconnects from the Redis server.
     */
    void disconnect();

    /**
     * @brief Sends a PING command to the Redis server.
     * 
     * @return The response string from the server (e.g., "PONG").
     */
    std::string ping();

    /**
     * @brief Sets a key to hold a string value with an optional expiration time.
     * 
     * @param key The key to set.
     * @param value The value to associate with the key.
     * @param expire_seconds Optional expiration time in seconds (0 means no expiration).
     * @return true if the command succeeded, false otherwise.
     */
    bool set(const std::string& key, const std::string& value, int expire_seconds = 0);

    /**
     * @brief Gets the value of a key.
     * 
     * @param key The key to query.
     * @return An optional string containing the value, or std::nullopt if the key does not exist.
     */
    std::optional<std::string> get(const std::string& key);

    /**
     * @brief Increments the integer value of a key by one.
     * 
     * @param key The key to increment.
     * @return The value of the key after the increment.
     */
    long long incr(const std::string& key);

    /**
     * @brief Sets a timeout on a key.
     * 
     * @param key The key to set the timeout on.
     * @param seconds The timeout in seconds.
     * @return true if the timeout was set, false otherwise.
     */
    bool expire(const std::string& key, int seconds);

private:
    // Callers must hold mutex_.
    bool connect_locked();
    void close_locked();

    std::string send_command(const std::vector<std::string>& args);
    std::string read_response(bool& ok);

    std::string host_;
    int port_;
    int fd_{-1};
    std::mutex mutex_; // Thread-safe for shared use across worker threads
};

} // namespace database
