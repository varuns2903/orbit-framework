#include <orbit/network/IocpProactor.hpp>
#include <orbit/utils/Logger.hpp>
#ifdef _WIN32
#include <algorithm>
#include <mswsock.h>
#include <io.h>
#include <stdexcept>
#include <iostream>

namespace orbit::network {

static LPFN_ACCEPTEX pAcceptEx = nullptr;
static LPFN_CONNECTEX pConnectEx = nullptr;
static LPFN_TRANSMITFILE pTransmitFile = nullptr;

static void load_extension_functions(socket_t fd) {
    if (pAcceptEx && pConnectEx && pTransmitFile) return;

    DWORD bytes;
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
             &pAcceptEx, sizeof(pAcceptEx), &bytes, NULL, NULL);

    GUID guidConnectEx = WSAID_CONNECTEX;
    WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidConnectEx, sizeof(guidConnectEx),
             &pConnectEx, sizeof(pConnectEx), &bytes, NULL, NULL);
             
    GUID guidTransmitFile = WSAID_TRANSMITFILE;
    WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidTransmitFile, sizeof(guidTransmitFile),
             &pTransmitFile, sizeof(pTransmitFile), &bytes, NULL, NULL);
}

IocpProactor::IocpProactor() {
    iocp_handle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocp_handle_ == NULL) {
        throw std::runtime_error("Failed to create IOCP");
    }
}

IocpProactor::~IocpProactor() {
    if (iocp_handle_) {
        CloseHandle(iocp_handle_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [fd, contexts] : pending_contexts_) {
        for (auto ctx : contexts) {
            delete ctx;
        }
    }
}

void IocpProactor::register_socket(socket_t fd) {
    if (CreateIoCompletionPort((HANDLE)fd, iocp_handle_, (ULONG_PTR)fd, 0) == NULL) {
        // May already be registered or failed
    }
    load_extension_functions(fd);
}

// An operation that failed to start is reported like any other failed
// completion, from run_once(): callers never see their callback run inside
// the call that started the operation.
void IocpProactor::fail_later(IocpContext* ctx) {
    ctx->failed = true;
    if (!PostQueuedCompletionStatus(iocp_handle_, 0, 0, &ctx->overlapped)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& vec = pending_contexts_[ctx->fd];
        vec.erase(std::remove(vec.begin(), vec.end(), ctx), vec.end());
        delete ctx;
    }
}

void IocpProactor::run_once(int timeout_ms) {
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    LPOVERLAPPED overlapped;

    BOOL result = GetQueuedCompletionStatus(
        iocp_handle_, &bytes_transferred, &completion_key, &overlapped, timeout_ms
    );

    if (overlapped == nullptr) {
        if (result == FALSE && GetLastError() != WAIT_TIMEOUT) {
            LOG_ERROR("IOCP error");
        }
        return;
    }

    IocpContext* ctx = reinterpret_cast<IocpContext*>(overlapped);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_contexts_.find(ctx->fd);
        if (it != pending_contexts_.end()) {
            auto& vec = it->second;
            for (auto vi = vec.begin(); vi != vec.end(); ++vi) {
                if (*vi == ctx) {
                    vec.erase(vi);
                    break;
                }
            }
        }
    }

    if (result == FALSE || ctx->failed) {
        // IO failed
        if (ctx->io_callback) ctx->io_callback(-1);
        if (ctx->accept_callback) ctx->accept_callback(INVALID_SOCKET_FD, sockaddr_in{});
        if (ctx->connect_callback) ctx->connect_callback(-1);
        if (ctx->type == IocpOperationType::ACCEPT && ctx->accept_socket != INVALID_SOCKET_FD) {
            close_socket(ctx->accept_socket);
        }
    } else {
        if (ctx->type == IocpOperationType::ACCEPT) {
            
            // Update context in accepted socket
            setsockopt(ctx->accept_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&ctx->fd, sizeof(ctx->fd));
            
            // With the accept context updated, getpeername() reports the
            // client address. Without it every client appeared as 0.0.0.0,
            // which broke per-IP rate limiting and X-Forwarded-For.
            sockaddr_in addr{};
            int addr_len = sizeof(addr);
            getpeername(ctx->accept_socket, reinterpret_cast<sockaddr*>(&addr), &addr_len);
            ctx->accept_callback(ctx->accept_socket, addr);
        } else if (ctx->type == IocpOperationType::CONNECT) {
            setsockopt(ctx->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
            ctx->connect_callback(0);
        } else {
            if (ctx->io_callback) {
                ctx->io_callback(bytes_transferred);
            }
        }
    }
    delete ctx;
}

void IocpProactor::async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    start_read(fd, buffer, size, 0, std::move(callback));
}

void IocpProactor::start_read(socket_t fd, void* buffer, size_t size, DWORD recv_flags, std::function<void(ssize_t)> callback) {
    register_socket(fd);
    auto ctx = new IocpContext();
    ctx->fd = fd;
    ctx->type = IocpOperationType::READ;
    ctx->io_callback = std::move(callback);
    ctx->wsa_buf.buf = static_cast<char*>(buffer);
    ctx->wsa_buf.len = static_cast<ULONG>(size);

    DWORD flags = recv_flags;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_contexts_[fd].push_back(ctx);
    }

    if (WSARecv(fd, &ctx->wsa_buf, 1, NULL, &flags, &ctx->overlapped, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            fail_later(ctx);
        }
    }
}

void IocpProactor::async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) {
    register_socket(fd);
    auto ctx = new IocpContext();
    ctx->fd = fd;
    ctx->type = IocpOperationType::WRITE;
    ctx->io_callback = std::move(callback);
    ctx->wsa_buf.buf = const_cast<char*>(static_cast<const char*>(buffer));
    ctx->wsa_buf.len = static_cast<ULONG>(size);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_contexts_[fd].push_back(ctx);
    }

    if (WSASend(fd, &ctx->wsa_buf, 1, NULL, 0, &ctx->overlapped, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            fail_later(ctx);
        }
    }
}

void IocpProactor::async_wait_read(socket_t fd, std::function<void()> callback) {
    // A zero-byte read completes when data arrives. On a datagram socket it
    // would also consume (and truncate away) the datagram, so peek instead,
    // as libuv does for UDP.
    int type = 0;
    int len = sizeof(type);
    bool datagram = getsockopt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == 0 && type == SOCK_DGRAM;
    start_read(fd, nullptr, 0, datagram ? MSG_PEEK : 0, [cb = std::move(callback)](ssize_t) { cb(); });
}

void IocpProactor::async_wait_write(socket_t fd, std::function<void()> callback) {
    // Zero byte send can signal writability
    async_write(fd, nullptr, 0, [cb = std::move(callback)](ssize_t) { cb(); });
}

void IocpProactor::async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) {
    register_socket(out_fd);
    
    // Convert int fd to Windows HANDLE
    HANDLE file_handle = (HANDLE)_get_osfhandle(in_fd);
    
    auto ctx = new IocpContext();
    ctx->fd = out_fd;
    ctx->type = IocpOperationType::SENDFILE;
    ctx->io_callback = std::move(callback);
    
    // Set overlapped offset
    ctx->overlapped.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFF);
    ctx->overlapped.OffsetHigh = static_cast<DWORD>((static_cast<uint64_t>(offset) >> 32) & 0xFFFFFFFF);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_contexts_[out_fd].push_back(ctx);
    }

    if (pTransmitFile && pTransmitFile(out_fd, file_handle, static_cast<DWORD>(count), 0, &ctx->overlapped, NULL, 0) == FALSE) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            fail_later(ctx);
        }
    }
}

void IocpProactor::async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) {
    register_socket(fd);
    auto ctx = new IocpContext();
    ctx->fd = fd;
    ctx->type = IocpOperationType::ACCEPT;
    ctx->accept_callback = std::move(callback);
    
    // AcceptEx needs a socket of the listener's address family (IPv6 for "::").
    sockaddr_storage listen_addr{};
    int listen_len = sizeof(listen_addr);
    int family = AF_INET;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&listen_addr), &listen_len) == 0) {
        family = listen_addr.ss_family;
    }
    socket_t client_sock = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    ctx->accept_socket = client_sock;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_contexts_[fd].push_back(ctx);
    }

    if (pAcceptEx) {
        DWORD received = 0;
        BOOL result = pAcceptEx(fd, client_sock, ctx->accept_buffer, 0, 
                                sizeof(sockaddr_storage) + 16, sizeof(sockaddr_storage) + 16, 
                                &received, &ctx->overlapped);
        if (result == FALSE && WSAGetLastError() != WSA_IO_PENDING) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_contexts_[fd].pop_back();
            close_socket(client_sock);
            delete ctx;
        }
    }
}

void IocpProactor::async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) {
    register_socket(fd);
    
    // ConnectEx requires socket to be bound first
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = 0;
    ::bind(fd, (sockaddr*)&bind_addr, sizeof(bind_addr));
    
    auto ctx = new IocpContext();
    ctx->fd = fd;
    ctx->type = IocpOperationType::CONNECT;
    ctx->connect_callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_contexts_[fd].push_back(ctx);
    }

    if (pConnectEx) {
        BOOL result = pConnectEx(fd, (const sockaddr*)&addr, sizeof(addr), NULL, 0, NULL, &ctx->overlapped);
        if (result == FALSE && WSAGetLastError() != WSA_IO_PENDING) {
            fail_later(ctx);
        }
    }
}

void IocpProactor::remove(socket_t fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_contexts_.find(fd);
    if (it != pending_contexts_.end()) {
        for (auto ctx : it->second) {
            // CancelIoEx can cancel specific overlapped operations
            CancelIoEx((HANDLE)fd, &ctx->overlapped);
        }
        // Let the GetQueuedCompletionStatus reap them
    }
}

} // namespace network
#endif // _WIN32
