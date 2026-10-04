#pragma once
#include <orbit/network/Socket.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/server/TimerManager.hpp>
#include <orbit/network/TlsContext.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <vector>
#include <chrono>
#include <string_view>
#include <memory>
#include <mutex>

namespace http::websocket { class WebSocketConnection; }
namespace http::h2 { class Http2Session; }

namespace server {

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
    ERROR_BAD_REQUEST
};

/**
 * @brief Timeouts applied to a connection, in the phases described on ServerConfig.
 */
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
    void on_removed();

    /**
     * @brief Sets the timeouts used for this connection. Call before start().
     */
    void set_timeouts(const ConnectionTimeouts& timeouts) { timeouts_ = timeouts; }

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
    void arm_timer(std::chrono::milliseconds timeout);
    void arm_timer_for_current_phase();
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
    std::string current_request_buffer_; // Holds the request data for string_views during async processing
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
    bool request_chunked_{false};  // decoding the incoming body (event-loop thread)
    bool response_chunked_{false}; // encoding the outgoing body (handler threads)
    bool is_head_request_{false}; // Responses to HEAD carry headers only
    bool is_chunk_header_mode_{true};
    size_t chunk_bytes_remaining_{0};
    size_t content_length_remaining_{0};

    RequestState check_request_state();
    std::string request_body_storage_; // Owns a decoded chunked request body
    std::atomic<bool> should_close_{false};
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
