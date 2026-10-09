#pragma once
#include <orbit/legacy_namespaces.hpp>

#include <nghttp3/nghttp3.h>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

namespace orbit::server {

class QuicConnection;
class Http3ResponseWriter;

namespace quic::detail {

/**
 * @brief Response body bytes handed to nghttp3, kept until the peer ACKs them.
 *
 * nghttp3 references the bytes it is given until acked_stream_data reports
 * them acknowledged, which happens in the order they were handed out. A
 * deque never moves its elements, so handed-out chunks stay put while more
 * are appended.
 */
class SentChunks {
public:
    /// Appends a chunk to send. Empty chunks are ignored.
    void push(std::string chunk);
    /// Points up to @p n vecs at chunks not handed out yet; returns how many.
    size_t hand_out(nghttp3_vec* vec, size_t n);
    /// Frees chunks whose bytes are all acknowledged.
    void ack(uint64_t bytes);
    /// True if some chunk has not been handed out yet.
    bool has_unsent() const { return handed_out_ < chunks_.size(); }
    /// Chunks still held (not yet handed out, or not yet acknowledged).
    size_t held() const { return chunks_.size(); }

private:
    std::deque<std::string> chunks_;
    size_t handed_out_ = 0;    // chunks at the front given to nghttp3
    uint64_t front_acked_ = 0; // acknowledged bytes of chunks_.front()
};

/// Owns the strings an nghttp3_nv list points into.
struct Http3HeaderBlock {
    std::vector<std::string> storage;
    std::vector<nghttp3_nv> nvs;
};

/// Encodes a response's header fields: `:status` first, names lowercased,
/// connection-specific fields dropped (RFC 9114 section 4.2).
Http3HeaderBlock build_response_headers(const http::HttpResponse& response);

} // namespace quic::detail

/// One request/response exchange on an HTTP/3 stream.
struct Http3Stream {
    int64_t stream_id;
    http::HttpRequest request;
    std::string path;   // :path as received (decoded at dispatch)
    std::string body;
    std::string cookie; // all cookie fields joined (they may arrive split)
    bool headers_complete = false;
    bool dispatched = false;
    bool body_too_large = false;

    quic::detail::SentChunks chunks; // response body
    bool streaming = false; // body produced by write_chunk() until end()
    bool ended = false;
    int file_fd = -1;       // file body: bytes [file_offset, file_end)
    int64_t file_offset = 0;
    int64_t file_end = 0;
    std::weak_ptr<Http3ResponseWriter> writer; // for is_open() / on_close() (#197)

    explicit Http3Stream(int64_t id) : stream_id(id) {}
    ~Http3Stream();
};

/**
 * @brief Manages an HTTP/3 session over a QUIC connection.
 *
 * Callbacks run on the event loop with the connection's mutex held; the
 * submit_* functions are called by Http3ResponseWriter from worker threads,
 * which take the same mutex first.
 */
class QuicHttp3Session {
public:
    /**
     * @brief Constructs a QuicHttp3Session.
     * @param quic_conn The underlying QUIC connection.
     */
    explicit QuicHttp3Session(QuicConnection& quic_conn);
    ~QuicHttp3Session();

    /**
     * @brief Initializes the HTTP/3 session.
     * @return true on success, false otherwise.
     */
    bool init();

    /**
     * @brief Gets the underlying nghttp3 connection.
     * @return Pointer to nghttp3_conn.
     */
    nghttp3_conn* get_conn() const { return httpconn_; }

    /// Feeds received stream data to nghttp3. Returns the bytes nghttp3
    /// consumed (owed back as flow-control credit), or a negative nghttp3 error.
    nghttp3_ssize process_stream_data(int64_t stream_id, const uint8_t* data, size_t datalen, bool fin);

    /// Submits a response. Takes ownership of response.file_fd when a file
    /// body is sent. With @p streaming, the body follows via submit_data()
    /// and end_stream(). Caller holds the connection mutex.
    void submit_response(int64_t stream_id, http::HttpResponse& response, bool has_body, bool streaming = false);
    void submit_data(int64_t stream_id, std::string_view chunk);
    void end_stream(int64_t stream_id);

private:
    QuicConnection& quic_conn_;
    nghttp3_conn* httpconn_{nullptr};
    std::unordered_map<int64_t, std::shared_ptr<Http3Stream>> streams_;

    // nghttp3 callbacks
    static int on_acked_stream_data(nghttp3_conn *conn, int64_t stream_id, uint64_t datalen, void *conn_user_data, void *stream_user_data);
    static int on_stream_close(nghttp3_conn *conn, int64_t stream_id, uint64_t app_error_code, void *conn_user_data, void *stream_user_data);
    static int on_recv_data(nghttp3_conn *conn, int64_t stream_id, const uint8_t *data, size_t datalen, void *conn_user_data, void *stream_user_data);
    static int on_deferred_consume(nghttp3_conn *conn, int64_t stream_id, size_t consumed, void *conn_user_data, void *stream_user_data);
    static int on_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data, void *stream_user_data);
    static int on_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token, nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t flags, void *conn_user_data, void *stream_user_data);
    static int on_end_headers(nghttp3_conn *conn, int64_t stream_id, int fin, void *conn_user_data, void *stream_user_data);
    static int on_end_stream(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data, void *stream_user_data);
    static nghttp3_ssize read_data(nghttp3_conn *conn, int64_t stream_id, nghttp3_vec *vec, size_t veccnt, uint32_t *pflags, void *conn_user_data, void *stream_user_data);

    std::shared_ptr<Http3Stream> get_or_create_stream(int64_t stream_id);
    void handle_request(std::shared_ptr<Http3Stream> stream);
    void submit_status(int64_t stream_id, http::HttpStatus status);
};

/**
 * @brief ResponseWriter for HTTP/3 streams.
 *
 * Handlers run on the thread pool and may finish after the client has gone,
 * so the writer refers to its connection only weakly.
 */
class Http3ResponseWriter : public http::ResponseWriter {
public:
    Http3ResponseWriter(std::weak_ptr<QuicConnection> conn, int64_t stream_id, network::Proactor& proactor,
                        concurrency::ThreadPool& thread_pool, bool suppress_body = false);

    void add_interceptor(Interceptor interceptor) override;
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
    std::weak_ptr<QuicConnection> conn_;
    int64_t stream_id_;
    network::Proactor& proactor_;
    concurrency::ThreadPool& thread_pool_;
    std::unordered_map<std::string, std::string> default_headers_;
    std::vector<Interceptor> interceptors_;
    bool headers_sent_{false};
    bool suppress_body_{false}; // HEAD request: headers only

    // The request body, kept alive by owner, for read_body_stream().
    std::shared_ptr<void> body_owner_;
    std::string_view body_;

    void apply_response_hooks(http::HttpResponse& response);
    /// Runs @p fn on the session with the connection locked, then flushes.
    template <typename Fn> void with_session(Fn&& fn);

    friend class QuicHttp3Session;
};

} // namespace server
