#include <orbit/network/Socket.hpp>
#include <stdexcept>
#include <string>
#include <cerrno>
#include <cstring>

namespace orbit::network {

Socket::Socket() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ == INVALID_SOCKET_FD) {
        throw std::runtime_error(std::string("Failed to create socket: ") + std::to_string(get_last_socket_error()));
    }
}

Socket::Socket(socket_t fd) : fd_(fd) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = INVALID_SOCKET_FD;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = INVALID_SOCKET_FD;
    }
    return *this;
}

socket_t Socket::fd() const {
    return fd_;
}

bool Socket::is_valid() const {
    return fd_ != INVALID_SOCKET_FD;
}

void Socket::close() {
    if (fd_ != INVALID_SOCKET_FD) {
        close_socket(fd_);
        fd_ = INVALID_SOCKET_FD;
    }
}

void Socket::set_non_blocking() {
    if (fd_ == INVALID_SOCKET_FD) return;
    network::set_non_blocking(fd_);
}

} // namespace network
