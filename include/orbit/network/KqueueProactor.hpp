#pragma once
#include <orbit/legacy_namespaces.hpp>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <orbit/network/Proactor.hpp>
#include <sys/event.h>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace orbit::network {

class KqueueProactor : public Proactor {
public:
    KqueueProactor();
    ~KqueueProactor() override;

    void run_once(int timeout_ms) override;

    void async_read(socket_t fd, void* buffer, size_t size, std::function<void(ssize_t)> callback) override;
    void async_write(socket_t fd, const void* buffer, size_t size, std::function<void(ssize_t)> callback) override;
    void async_wait_read(socket_t fd, std::function<void()> callback) override;
    void async_wait_write(socket_t fd, std::function<void()> callback) override;
    void async_sendfile(socket_t out_fd, int in_fd, off_t offset, size_t count, std::function<void(ssize_t)> callback) override;
    void async_accept(socket_t fd, std::function<void(socket_t, sockaddr_in)> callback) override;
    void async_connect(socket_t fd, const sockaddr_in& addr, std::function<void(int)> callback) override;

    void remove(socket_t fd) override;

private:
    struct Context {
        int fd{-1};
        bool tracked{false};
        
        bool reading{false};
        void* read_buf{nullptr};
        size_t read_size{0};
        std::function<void(ssize_t)> read_cb;

        bool writing{false};
        const void* write_buf{nullptr};
        size_t write_size{0};
        std::function<void(ssize_t)> write_cb;
        
        bool waiting_read{false};
        std::function<void()> wait_read_cb;
        
        bool waiting_write{false};
        std::function<void()> wait_write_cb;
        
        bool sendfile_in_progress{false};
        int sendfile_in_fd{-1};
        off_t sendfile_offset{0};
        size_t sendfile_count{0};
        std::function<void(ssize_t)> sendfile_cb;

        bool accepting{false};
        std::function<void(socket_t, sockaddr_in)> accept_cb;
        
        bool connecting{false};
        std::function<void(int)> connect_cb;
    };

    void update_kqueue(Context& ctx);
    void handle_event(const struct kevent& event);

    int kq_fd_;
    std::unordered_map<int, Context> contexts_;
    std::mutex ctx_mutex_;
    std::vector<struct kevent> events_;
};

} // namespace network
#endif
