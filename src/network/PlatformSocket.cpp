#include <orbit/network/PlatformSocket.hpp>
#include <stdexcept>
#include <string>

#include <cstring>

namespace network {

void initialize_platform_networking() {
#ifdef _WIN32
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error: " + std::to_string(result));
    }
#endif
}

void cleanup_platform_networking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void close_socket(socket_t fd) {
    if (fd == INVALID_SOCKET_FD) return;
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

void set_non_blocking(socket_t fd) {
    if (fd == INVALID_SOCKET_FD) return;
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != NO_ERROR) {
        throw std::runtime_error("ioctlsocket FIONBIO failed with error: " + std::to_string(get_last_socket_error()));
    }
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error(std::string("fcntl F_GETFL failed: ") + std::strerror(errno));
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error(std::string("fcntl F_SETFL O_NONBLOCK failed: ") + std::strerror(errno));
    }
#endif
}

std::string format_ip(const sockaddr* addr) {
    char buf[INET6_ADDRSTRLEN] = {};
    if (addr->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(addr);
        if (!inet_ntop(AF_INET, &v4->sin_addr, buf, sizeof(buf))) return "";
        return buf;
    }
    if (addr->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(addr);
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            in_addr v4{};
            std::memcpy(&v4, reinterpret_cast<const unsigned char*>(&v6->sin6_addr) + 12, sizeof(v4));
            if (!inet_ntop(AF_INET, &v4, buf, sizeof(buf))) return "";
            return buf;
        }
        if (!inet_ntop(AF_INET6, &v6->sin6_addr, buf, sizeof(buf))) return "";
        return buf;
    }
    return "";
}

std::string peer_ip(socket_t fd) {
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "";
    return format_ip(reinterpret_cast<const sockaddr*>(&addr));
}

} // namespace network
