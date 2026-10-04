#include <orbit/server/Listener.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <memory>
#include <orbit/utils/Logger.hpp>

namespace server {

Listener::Listener(uint16_t port) : Listener("0.0.0.0", port) {}

Listener::Listener(std::string host, uint16_t port, int backlog)
    : host_(host.empty() ? "0.0.0.0" : std::move(host)), port_(port), backlog_(backlog) {}

namespace {

void set_flag(network::socket_t fd, int level, int option, int value) {
#ifdef _WIN32
    setsockopt(fd, level, option, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    setsockopt(fd, level, option, &value, sizeof(value));
#endif
}

// Creates, configures and binds a socket for one resolved address.
network::Socket bind_address(const addrinfo& ai, int& error) {
    network::Socket socket(::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol));
    if (!socket.is_valid()) {
        error = network::get_last_socket_error();
        return socket;
    }

    int opt = 1;
#ifdef _WIN32
    if (setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == network::SOCKET_ERROR_VAL) {
        throw std::runtime_error(std::string("setsockopt SO_REUSEADDR failed: ") + std::to_string(network::get_last_socket_error()));
    }
#else
    if (setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == network::SOCKET_ERROR_VAL) {
        throw std::runtime_error(std::string("setsockopt SO_REUSEADDR failed: ") + std::to_string(network::get_last_socket_error()));
    }
    
    // Enable SO_REUSEPORT for zero-downtime hot reloading (POSIX only)
    if (setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == network::SOCKET_ERROR_VAL) {
        LOG_ERROR("setsockopt SO_REUSEPORT failed (Hot Reloading may not work): " << std::to_string(network::get_last_socket_error()));
    }
#endif

    if (ai.ai_family == AF_INET6) {
        // "::" should also accept IPv4 clients (as ::ffff:a.b.c.d). The
        // default differs between platforms, so set it explicitly.
        set_flag(socket.fd(), IPPROTO_IPV6, IPV6_V6ONLY, 0);
    }

    // Set listening socket to non-blocking
    socket.set_non_blocking();

    if (::bind(socket.fd(), ai.ai_addr, static_cast<network::socklen_t>(ai.ai_addrlen)) == network::SOCKET_ERROR_VAL) {
        error = network::get_last_socket_error();
        socket.close();
    }
    return socket;
}

} // namespace

void Listener::start() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    addrinfo* results = nullptr;
    std::string service = std::to_string(port_);
    int rc = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &results);
    if (rc != 0) {
        throw std::runtime_error("cannot resolve listen address '" + host_ + "': " + gai_strerror(rc));
    }
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(results, &::freeaddrinfo);

    // Use the first address that binds (e.g. "localhost" may resolve to ::1
    // and 127.0.0.1; either serves local clients).
    int error = 0;
    for (addrinfo* ai = results; ai; ai = ai->ai_next) {
        network::Socket candidate = bind_address(*ai, error);
        if (candidate.is_valid()) {
            socket_ = std::move(candidate);
            break;
        }
    }
    if (!socket_.is_valid()) {
        throw std::runtime_error("bind to " + host_ + ":" + service + " failed: " + std::to_string(error));
    }

#ifdef SOMAXCONN
    const int backlog = backlog_ > 0 ? backlog_ : SOMAXCONN;
#else
    const int backlog = backlog_ > 0 ? backlog_ : 511;
#endif
    if (::listen(socket_.fd(), backlog) == network::SOCKET_ERROR_VAL) {
        throw std::runtime_error(std::string("listen failed: ") + std::to_string(network::get_last_socket_error()));
    }

    LOG_INFO("Listening on " << host_ << " port " << port() << "...");
}

std::optional<network::Socket> Listener::accept_connection() {
    sockaddr_storage client_addr{};
    network::socklen_t client_addr_len = sizeof(client_addr);
    
    network::socket_t client_fd = ::accept(socket_.fd(), reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
    if (client_fd == network::INVALID_SOCKET_FD) {
        if (network::is_would_block_error()) {
            return std::nullopt;
        }
        throw std::runtime_error(std::string("accept failed: ") + std::to_string(network::get_last_socket_error()));
    }

    LOG_DEBUG("Accepted connection from " << network::format_ip(reinterpret_cast<sockaddr*>(&client_addr)));

    return network::Socket(client_fd);
}

int Listener::fd() const {
    return static_cast<int>(socket_.fd());
}

void Listener::close() {
    socket_.close();
}

uint16_t Listener::port() const {
    sockaddr_storage addr{};
    network::socklen_t len = sizeof(addr);
    if (!socket_.is_valid() || getsockname(socket_.fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return port_;
    }
    if (addr.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    }
    return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
}

} // namespace server
