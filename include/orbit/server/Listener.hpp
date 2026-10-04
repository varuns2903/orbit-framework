#pragma once
#include <orbit/network/Socket.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace server {

/**
 * @brief Listens for incoming network connections.
 */
class Listener {
public:
    /**
     * @brief Constructs a Listener on the specified port, on all IPv4 interfaces.
     * @param port The port to listen on.
     */
    explicit Listener(uint16_t port);

    /**
     * @brief Constructs a Listener on a specific address.
     * @param host Address or host name to bind: "0.0.0.0" (all IPv4), "::"
     *             (all IPv6 and IPv4, dual-stack), "127.0.0.1", "::1",
     *             "localhost", ... An empty string means "0.0.0.0".
     * @param port The port to listen on; 0 picks a free port (see port()).
     * @param backlog Pending-connection queue length; 0 or less uses SOMAXCONN.
     */
    Listener(std::string host, uint16_t port, int backlog = 0);
    
    /**
     * @brief Binds and starts listening.
     * @throws std::runtime_error if the address cannot be resolved or bound.
     */
    void start();

    /**
     * @brief Accepts a new incoming connection.
     * @return An optional Socket if a connection was accepted, std::nullopt otherwise.
     */
    std::optional<network::Socket> accept_connection();
    
    /**
     * @brief Gets the underlying file descriptor for epoll registration.
     * @return The file descriptor.
     */
    int fd() const;

    /**
     * @brief The port actually bound (useful when constructed with port 0).
     */
    uint16_t port() const;

    /**
     * @brief Closes the listening socket, so new connections are refused
     *        instead of queueing in the backlog (used when draining).
     */
    void close();

private:
    std::string host_;
    uint16_t port_;
    int backlog_;
    network::Socket socket_{network::INVALID_SOCKET_FD};
};

} // namespace server
