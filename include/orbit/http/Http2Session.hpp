#pragma once

#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <nghttp2/nghttp2.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <mutex>
#include <string_view>

namespace server {
    class Connection;
}

namespace http {
namespace h2 {

namespace detail {

/**
 * @brief Owns the header name/value strings backing an nghttp2_nv list.
 *
 * nghttp2_nv stores borrowed pointers, so every string it references must
 * outlive the nghttp2_submit_* call that consumes the list. Building the list
 * through this type keeps that storage alive in one place instead of relying
 * on incidental lifetimes at the call site.
 */
struct HeaderBlock {
    std::vector<std::string> storage;
    std::vector<nghttp2_nv> nvs;
};

/**
 * @brief Builds the HTTP/2 response header list for a response.
 *
 * Emits the mandatory `:status` pseudo-header first, then each response
 * header with its name lowercased as HTTP/2 requires. Connection-specific
 * headers that are forbidden in HTTP/2 (RFC 9113 section 8.2.2) are dropped.
 *
 * @param response The response whose headers should be encoded.
 * @return A HeaderBlock owning the encoded names/values and the nv list.
 */
HeaderBlock build_response_headers(const http::HttpResponse& response);

/**
 * @brief Maps an HTTP/2 `:method` pseudo-header value to an HttpMethod.
 * @param value The `:method` value as received on the wire.
 * @param out Set to the parsed method on success; untouched on failure.
 * @return True if the method was recognised.
 */
bool parse_method(std::string_view value, http::HttpMethod& out);

/**
 * @brief True if a header is forbidden in HTTP/2 and must not be forwarded.
 * @param name The header name, in any case.
 */
bool is_connection_specific_header(std::string_view name);

} // namespace detail

/**
 * @brief Manages an HTTP/2 session over a connection.
 */
class Http2Session {
public:
    Http2Session(server::Connection& connection, const routing::Router& router, concurrency::ThreadPool& thread_pool);
    ~Http2Session();

    void process_data(const uint8_t* data, size_t len);
    void send_pending();
    
    /**
     * @brief Thread-safe API to submit an HTTP/2 response.
     * @param stream_id The HTTP/2 stream identifier.
     * @param response The HttpResponse to send.
     * @param has_body True if the response includes a body.
     */
    void submit_response(int32_t stream_id, const http::HttpResponse& response, bool has_body);
    void submit_data(int32_t stream_id);
    void end_stream(int32_t stream_id);

    server::Connection& get_connection() { return connection_; }

private:
    server::Connection& connection_;
    const routing::Router& router_;
    concurrency::ThreadPool& thread_pool_;
    
    nghttp2_session* session_{nullptr};
    std::mutex session_mutex_;

    struct StreamContext {
        int32_t stream_id;
        http::HttpRequest request;
        
        // Backing storage for string_views
        std::string backing_uri;
        std::string backing_body;
        std::vector<std::pair<std::string, std::string>> backing_headers;
        
        // For writing response bodies
        std::string response_body;
        size_t response_offset{0};
        
        // For sendfile
        int file_fd{-1};
        off_t file_size{0};
        off_t file_offset{0};
        
        Http2Session* session;
    };

    std::unordered_map<int32_t, std::shared_ptr<StreamContext>> streams_;

    static int on_begin_headers(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_header(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data);
    static int on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame, void* user_data);
    static int on_data_chunk_recv(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data);
    static int on_stream_close(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data);
    static ssize_t send_callback(nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data);
    
    static ssize_t data_provider_read(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data);

    void dispatch_request(std::shared_ptr<StreamContext> stream_ctx);
};

/**
 * @brief ResponseWriter implementation for HTTP/2 streams.
 */
class Http2ResponseWriter : public http::ResponseWriter {
public:
    Http2ResponseWriter(Http2Session* session, int32_t stream_id);
    
    void add_interceptor(Interceptor interceptor) override;
    void set_header(const std::string& key, const std::string& value) override;
    network::Proactor& proactor() override;
    concurrency::ThreadPool& thread_pool() override;
    
    void send(http::HttpResponse&& response) override;
    void send_headers(http::HttpResponse& response) override;
    void write_chunk(std::string_view chunk) override;
    void end() override;
    void send_sse_event(std::string_view data, std::string_view event = "", std::string_view id = "") override;
    void upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) override;
    void read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) override;

private:
    Http2Session* session_;
    int32_t stream_id_;
    std::unordered_map<std::string, std::string> default_headers_;
    std::vector<Interceptor> interceptors_;
    bool headers_sent_{false};
};

} // namespace h2
} // namespace http
