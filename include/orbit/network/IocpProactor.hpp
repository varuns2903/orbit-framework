#pragma once
#include <orbit/network/Proactor.hpp>

#ifdef _WIN32
#include <unordered_map>
#include <mutex>
#include <vector>

namespace network {

enum class IocpOperationType {
    READ,
    WRITE,
    ACCEPT,
    CONNECT,
    SENDFILE
};

struct IocpContext {
    OVERLAPPED overlapped;
    socket_t fd;
    IocpOperationType type;
    std::function<void(ssize_t)> io_callback;
    std::function<void(socket_t, sockaddr_in)> accept_callback;
    std::function<void(int)> connect_callback;
    
    // For AcceptEx
    socket_t accept_socket{INVALID_SOCKET_FD};
    // Room for IPv4 or IPv6 local and remote addresses.
    char accept_buffer[(sizeof(sockaddr_storage) + 16) * 2];
    
    // For WSARecv/WSASend
    WSABUF wsa_buf;
    
    IocpContext() {
        memset(&overlapped, 0, sizeof(OVERLAPPED));
    }
};

class IocpProactor : public Proactor {
public:
    IocpProactor();
    ~IocpProactor() override;

    void run_once(int timeout_ms) override;

    void async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t bytes_read)> callback) override;
    void async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t bytes_written)> callback) override;
    
    // IOCP does not directly support readiness polling without reading/writing, 
    // so we emulate it using zero-byte reads or event selection if necessary.
    void async_wait_read(socket_t fd, std::function<void()> callback) override;
    void async_wait_write(socket_t fd, std::function<void()> callback) override;
    
    void async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t bytes_written)> callback) override;
    void async_accept(socket_t fd, std::function<void(socket_t new_fd, sockaddr_in addr)> callback) override;
    void async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int status)> callback) override;

    void remove(socket_t fd) override;

private:
    void register_socket(socket_t fd);

    HANDLE iocp_handle_;
    std::mutex mutex_;
    std::unordered_map<socket_t, std::vector<IocpContext*>> pending_contexts_;
};

} // namespace network
#endif // _WIN32
