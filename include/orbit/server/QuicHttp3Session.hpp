#pragma once

#include <nghttp3/nghttp3.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>

namespace server {

class QuicConnection;

struct Http3Stream {
    int64_t stream_id;
    http::HttpRequest request;
    std::string body_buffer;
    bool headers_complete{false};
    std::string response_body; // To store response payload
    
    Http3Stream(int64_t id) : stream_id(id) {}
};

/**
 * @brief Manages an HTTP/3 session over a QUIC connection.
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
    
    /**
     * @brief Processes received QUIC stream data for HTTP/3.
     * @param stream_id The QUIC stream ID.
     * @param data The stream data.
     * @param datalen The length of the data.
     * @param fin Whether this is the final data for the stream.
     * @return 0 on success, or a negative error code.
     */
    int process_stream_data(int64_t stream_id, const uint8_t* data, size_t datalen, bool fin);
    
    /**
     * @brief Serializes HTTP/3 frames and feeds them to ngtcp2.
     * @return 0 on success, or a negative error code.
     */
    int write_streams();

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
    
    std::shared_ptr<Http3Stream> get_or_create_stream(int64_t stream_id);
    void handle_request(std::shared_ptr<Http3Stream> stream);
};

} // namespace server
