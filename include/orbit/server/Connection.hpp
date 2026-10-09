#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/network/Socket.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/server/TimerManager.hpp>
#include <orbit/network/TlsContext.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/http/Http1Parser.hpp>
#include <vector>
#include <chrono>
#include <string_view>
#include <memory>
#include <mutex>

namespace orbit::http::websocket { class WebSocketConnection; }
namespace orbit::http::h2 { class Http2Session; }

namespace orbit::server {

class ConnectionManager; // Forward declaration

enum class ConnectionState {
    HTTP,
    HTTP_STREAMING_BODY,
    WEBSOCKET,
    RAW_STREAM,
    HTTP2
};

enum class RequestState {
    INCOMPLETE,
    COMPLETE,
    HEADERS_COMPLETE,
    ERROR_PAYLOAD_TOO_LARGE,
    ERROR_HEADERS_TOO_LARGE,
    ERROR_BAD_REQUEST,
    ERROR_NOT_IMPLEMENTED
};

/**
 * @brief Timeouts applied to a connection, in the phases described on ServerConfig.
 */
/**
 * @brief Request size limits for one connection (see ServerConfig).
 */
struct ConnectionLimits {
    size_t max_header_bytes = 8192;   ///< Request line plus headers
    size_t max_request_line = 4096;
    size_t max_headers = 100;         ///< Header fields per request
    size_t websocket_max_message_size = 16 * 1024 * 1024;
    /// Accept HTTP/2 with prior knowledge (h2c) on plaintext connections.
    bool h2c = false;
};

struct ConnectionTimeouts {
    std::chrono::milliseconds header{10000};
    std::chrono::milliseconds keep_alive{10000};
    std::chrono::milliseconds idle{30000};
    std::chrono::milliseconds websocket_idle{0};
};

/**
 * @brief Represents an active client connection, handling request parsing and response writing.
 */
class Connection : public std::enable_shared_from_this<Connection>, public http::ResponseWriter {
public:
    /**
     * @brief Constructs a new Connection.
     * @param socket The connection socket.
     * @param client_ip The client's IP address.
     * @param proactor The Proactor for async I/O.
     * @param router The application router.
     * @param manager The connection manager.
     * @param thread_pool The application thread pool.
     * @param timer_manager The timer manager for timeouts.
     * @param max_body_size Maximum allowed request body size.
     * @param tls_context The TLS context, if applicable.
     */
    Connection(network::Socket socket, const std::string& client_ip, network::Proactor& proactor, const routing::Router& router, ConnectionManager& manager, concurrency::ThreadPool& thread_pool, TimerManager& timer_manager, size_t max_body_size, network::TlsContext* tls_context = nullptr);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /**
     * @brief Starts processing the connection.
     */
    void start();

    /// Called by ConnectionManager when the connection is dropped, before
    /// its pending I/O is cancelled. No new I/O is submitted afterwards.
    /// Writers held elsewhere then report closed (on_close callbacks): on a
    /// pool thread, or right away when @p notify_now (the server is going
    /// away and the pool with it).
    void on_removed(bool notify_now = false);

    /**
     * @brief Sets the timeouts used for this connection. Call before start().
     */
    void set_timeouts(const ConnectionTimeouts& timeouts) { timeouts_ = timeouts; }
    void set_limits(const ConnectionLimits& limits) { limits_ = limits; }

    /// Event-loop thread: sends a WebSocket ping if this is a WebSocket.
    void ping_if_websocket();

    /**
     * @brief Writes raw data to the connection.
     * @param data The raw data to write.
     */
    void write_raw(const std::vector<char>& data);
    
    /**
     * @brief Marks the connection to be closed after writing completes.
     */
    void mark_for_close();

    /// Event-loop thread, repeatedly while the server drains: closes the
    /// connection if it is idle, otherwise makes its current work the last
    /// (Connection: close, HTTP/2 GOAWAY, WebSocket close 1001).
    void on_server_shutdown();
    
    /**
     * @brief Upgrades the connection to a WebSocket.
     * @param ws_conn The WebSocket connection instance.
     */
    void upgrade_to_websocket(std::unique_ptr<http::websocket::WebSocketConnection> ws_conn);
    
    /**
     * @brief Gets the client's IP address.
     * @return The IP address string.
     */
    const std::string& client_ip() const { return client_ip_; }

    // ResponseWriter Implementation
    void add_interceptor(std::function<void(http::HttpResponse&)> interceptor) override;
    void set_header(const std::string& key, const std::string& value) override;
    network::Proactor& proactor() override { return proactor_; }
    concurrency::ThreadPool& thread_pool() override { return thread_pool_; }
    void send(http::HttpResponse&& response) override;
    void send_headers(http::HttpResponse& response) override;
    void write_chunk(std::string_view chunk) override;
    void end() override;
    void send_sse_event(std::string_view data, std::string_view event = "", std::string_view id = "") override;
    void upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) override;
    void read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) override;

private:
    void process_request();
    // Queues bytes for writing; with close_after, closes once they are sent.
    void send_data(std::string_view data, bool close_after = false);
    /// Sends "100 Continue" once per request to a client that asked for it.
    void send_continue_if_expected();
    void arm_timer(std::chrono::milliseconds timeout);
    void arm_timer_for_current_phase();
    // WebSocket or raw stream: long-lived, timed out by peer inactivity only.
    bool is_message_stream() const;
    void send_error(http::HttpStatus status, const std::string& message);

    void trigger_read();
    void on_read_complete(ssize_t bytes_read);
    void trigger_write();
    void on_write_complete(ssize_t bytes_written);
    void on_sendfile_complete(ssize_t bytes_written);
    void process_streaming_data();

    network::Socket socket_;
    std::string client_ip_;
    network::Proactor& proactor_;
    const routing::Router& router_;
    ConnectionManager& manager_;
    concurrency::ThreadPool& thread_pool_;
    TimerManager& timer_manager_;
    
    std::vector<char> read_buffer_;
    mutable std::mutex read_mutex_;
    std::vector<char> write_buffer_;
    std::vector<char> active_write_buffer_;
    mutable std::mutex write_mutex_;
    
    char async_read_buf_[16384]; // Buffer for kernel to write into asynchronously

    std::unordered_map<std::string, std::string> default_headers_; // Populated by middlewares; guarded by write_mutex_
    std::unordered_map<std::string, std::string> default_headers_snapshot();
    std::atomic<bool> is_reading_{false};

    // Runs submit() (one proactor_.async_* call) unless the connection has
    // been removed. A worker thread can finish a response after the event
    // loop already removed the connection; registering I/O then would keep
    // the socket (held by the callback's shared_ptr) open until the peer
    // gave up. Returns false if nothing was submitted.
    template <typename Submit>
    bool submit_io(Submit&& submit) {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (removed_) return false;
        submit();
        return true;
    }
    std::mutex io_mutex_;
    bool removed_ = false; // guarded by io_mutex_
    std::atomic<bool> is_writing_{false};
    // Separate flags: a handler may start a chunked response while a
    // chunked request body is still being streamed in.
    bool response_chunked_{false}; // encoding the outgoing body (handler threads)
    bool is_head_request_{false}; // Responses to HEAD carry headers only

    RequestState check_request_state();
    // HTTP/1.1 request parsing. Guarded by read_mutex_; fed only while no
    // handler is using the request it holds (except a streamed body).
    std::unique_ptr<http::Http1Parser> parser_;
    bool message_started_ = false;  // bytes of the current request were parsed
    bool headers_parsed_ = false;   // its headers are complete, a body follows
    bool request_complete_ = false; // it is whole, waiting to be handled
    bool streaming_request_ = false; // a stream route: the handler reads the body
    std::atomic<bool> should_close_{false};
    bool h2c_decided_ = false; // event-loop thread: the connection's first bytes were checked
    // Expect: 100-continue on the request being read, and whether the 100
    // has been sent for it.
    std::atomic<bool> expect_continue_{false};
    std::atomic<bool> continue_sent_{false};
    // Set (under write_mutex_) together with the final response's bytes;
    // the connection closes when everything queued has been written.
    bool close_after_write_{false};
    // Set (under write_mutex_) while a file response is being sent; the
    // next pipelined request waits for it.
    bool resume_after_write_{false};
    // Written by handler threads (upgrades, streaming) and read by the event loop.
    std::atomic<ConnectionState> state_{ConnectionState::HTTP};
    uint64_t current_timer_id_{0};
    std::mutex timer_mutex_;
    ConnectionTimeouts timeouts_;
    ConnectionLimits limits_;
    
    int file_fd_{-1};
    off_t file_size_{0};
    off_t file_offset_{0};
    
    size_t max_body_size_;
    
    SSL* ssl_{nullptr};
    BIO* rbio_{nullptr};
    BIO* wbio_{nullptr};
    bool is_tls_handshake_complete_{false};
    std::vector<char> tls_write_buffer_; // For holding ciphertext before sending
    // OpenSSL objects are not thread-safe: SSL_read runs on the event loop and
    // SSL_write on whichever thread sends. tls_mutex_ guards ssl_, the BIOs and
    // tls_write_buffer_ (encrypted bytes waiting to be written).
    std::mutex tls_mutex_;
    // Encrypted bytes handed to the proactor. Only the thread holding
    // is_writing_ touches it, so it is never reallocated during a write.
    std::vector<char> tls_inflight_;
    void drain_tls_output_locked();
    // True if anything is queued that trigger_write() would send. Also
    // reports, consistently with the queue, whether a close was requested
    // and (when nothing is pending) whether a finished file response is
    // waiting to resume the request pipeline.
    bool has_pending_output(bool& close_requested, bool& resume);
    void continue_after_response();
    
    std::atomic<bool> is_processing_request_{false};
    bool shutdown_notified_ = false; // event-loop thread only
    
    std::unique_ptr<http::websocket::WebSocketConnection> ws_connection_;
    std::shared_ptr<http::h2::Http2Session> h2_session_;
    std::function<void(std::string_view)> raw_stream_on_data_;
    std::function<void()> raw_stream_on_close_;
    std::function<void(std::string_view)> body_stream_on_data_;
    std::function<void()> body_stream_on_end_;
    std::vector<std::function<void(http::HttpResponse&)>> interceptors_;
};

} // namespace server
