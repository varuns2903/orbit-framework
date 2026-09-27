#include <orbit/http/Http2Session.hpp>
#ifdef _WIN32
#include <io.h>
#define close _close
#define open _open
inline ssize_t pread(int fd, void* buf, size_t count, long offset) {
    _lseek(fd, offset, SEEK_SET);
    return _read(fd, buf, static_cast<unsigned int>(count));
}
#endif
#include <orbit/server/Connection.hpp>
#include <iostream>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/types.h>

#ifdef _WIN32
#undef DELETE
#undef ERROR
#endif

#include <cctype>
#include <stdexcept>

namespace http {
namespace h2 {

namespace detail {

bool is_connection_specific_header(std::string_view name) {
    // RFC 9113 section 8.2.2: connection-specific header fields must not be
    // used in HTTP/2. An endpoint that receives them must treat the message
    // as malformed, so we never emit them.
    static constexpr std::string_view forbidden[] = {
        "connection", "transfer-encoding", "keep-alive",
        "proxy-connection", "upgrade"
    };
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name) lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (std::string_view f : forbidden) {
        if (lowered == f) return true;
    }
    return false;
}

bool parse_method(std::string_view value, http::HttpMethod& out) {
    if (value == "GET") { out = http::HttpMethod::GET; return true; }
    if (value == "POST") { out = http::HttpMethod::POST; return true; }
    if (value == "PUT") { out = http::HttpMethod::PUT; return true; }
    if (value == "DELETE") { out = http::HttpMethod::DELETE; return true; }
    if (value == "PATCH") { out = http::HttpMethod::PATCH; return true; }
    if (value == "OPTIONS") { out = http::HttpMethod::OPTIONS; return true; }
    if (value == "HEAD") { out = http::HttpMethod::HEAD; return true; }
    return false;
}

HeaderBlock build_response_headers(const http::HttpResponse& response) {
    HeaderBlock block;

    // Reserve up front so the strings never move: nghttp2_nv borrows pointers
    // into this storage, and a reallocation would invalidate every entry
    // already pushed.
    block.storage.reserve(1 + response.headers.size() * 2);
    block.nvs.reserve(1 + response.headers.size());

    block.storage.push_back(std::to_string(static_cast<int>(response.status_code)));
    const std::string& status_str = block.storage.back();

    static constexpr std::string_view kStatus = ":status";
    block.nvs.push_back({
        reinterpret_cast<uint8_t*>(const_cast<char*>(kStatus.data())),
        reinterpret_cast<uint8_t*>(const_cast<char*>(status_str.data())),
        kStatus.size(),
        status_str.size(),
        NGHTTP2_NV_FLAG_NONE
    });

    for (const auto& [k, v] : response.headers) {
        if (is_connection_specific_header(k)) continue;

        std::string lower_k;
        lower_k.reserve(k.size());
        for (char c : k) lower_k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        block.storage.push_back(std::move(lower_k));
        const std::string& name = block.storage.back();
        block.storage.push_back(v);
        const std::string& value = block.storage.back();

        block.nvs.push_back({
            reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
            name.size(),
            value.size(),
            NGHTTP2_NV_FLAG_NONE
        });
    }

    return block;
}

} // namespace detail

// ---------------- Http2Session ----------------

Http2Session::Http2Session(std::weak_ptr<server::Connection> connection, network::Proactor& proactor,
                           const routing::Router& router, concurrency::ThreadPool& thread_pool,
                           std::string client_ip, size_t max_body_size)
    : connection_(std::move(connection)), proactor_(proactor), router_(router), thread_pool_(thread_pool),
      client_ip_(std::move(client_ip)), max_body_size_(max_body_size) {
    
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    
    nghttp2_session_server_new(&session_, callbacks, this);
    nghttp2_session_callbacks_del(callbacks);
    
    // Submit initial settings
    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };
    nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
    send_pending();
}

Http2Session::~Http2Session() {
    if (session_) {
        nghttp2_session_del(session_);
    }
    for (auto& [id, ctx] : streams_) {
        if (ctx->file_fd != -1) {
            close(ctx->file_fd);
            ctx->file_fd = -1;
        }
    }
}

void Http2Session::process_data(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    ssize_t rv = nghttp2_session_mem_recv(session_, data, len);
    if (rv < 0) {
        if (auto conn = connection_.lock()) conn->mark_for_close();
        return;
    }
    send_pending();
}

void Http2Session::send_pending() {
    nghttp2_session_send(session_);
}

int Http2Session::on_begin_headers(nghttp2_session* session, const nghttp2_frame* frame, void* user_data) {
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        auto* self = static_cast<Http2Session*>(user_data);
        auto stream_ctx = std::make_shared<StreamContext>();
        stream_ctx->stream_id = frame->hd.stream_id;
        self->streams_[frame->hd.stream_id] = stream_ctx;
    }
    return 0;
}

int Http2Session::on_header(nghttp2_session* session, const nghttp2_frame* frame, const uint8_t* name, size_t namelen, const uint8_t* value, size_t valuelen, uint8_t flags, void* user_data) {
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    auto* self = static_cast<Http2Session*>(user_data);
    auto it = self->streams_.find(frame->hd.stream_id);
    if (it == self->streams_.end()) return 0;
    
    std::string key(reinterpret_cast<const char*>(name), namelen);
    std::string val(reinterpret_cast<const char*>(value), valuelen);
    
    auto& ctx = *it->second;
    auto& req = ctx.request;
    if (key == ":method") {
        detail::parse_method(val, req.method);
    } else if (key == ":path") {
        ctx.backing_uri = val;
        req.uri = ctx.backing_uri;
    } else if (key == ":authority") {
        ctx.backing_headers.push_back({"Host", val});
    } else if (!key.empty() && key[0] != ':') {
        ctx.backing_headers.push_back({key, val});
    }
    return 0;
}

int Http2Session::on_frame_recv(nghttp2_session* session, const nghttp2_frame* frame, void* user_data) {
    auto* self = static_cast<Http2Session*>(user_data);
    auto it = self->streams_.find(frame->hd.stream_id);
    if (it == self->streams_.end()) return 0;
    
    bool ends_request = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        ((frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) ||
         frame->hd.type == NGHTTP2_DATA);
    if (ends_request) {
        if (it->second->body_too_large) {
            self->submit_status_locked(frame->hd.stream_id, http::HttpStatus::PayloadTooLarge);
        } else {
            self->dispatch_request(it->second);
        }
    }
    return 0;
}

int Http2Session::on_data_chunk_recv(nghttp2_session* session, uint8_t flags, int32_t stream_id, const uint8_t* data, size_t len, void* user_data) {
    (void)session;
    (void)flags;
    (void)user_data;
    auto* self = static_cast<Http2Session*>(user_data);
    auto it = self->streams_.find(stream_id);
    if (it == self->streams_.end()) return 0;
    
    auto& ctx = *it->second;
    if (ctx.body_too_large) return 0;
    if (ctx.backing_body.size() + len > self->max_body_size_) {
        // Stop buffering; the request is answered with 413 when it ends.
        ctx.body_too_large = true;
        ctx.backing_body.clear();
        ctx.backing_body.shrink_to_fit();
        return 0;
    }
    ctx.backing_body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

int Http2Session::on_stream_close(nghttp2_session* session, int32_t stream_id, uint32_t error_code, void* user_data) {
    (void)session;
    (void)error_code;
    auto* self = static_cast<Http2Session*>(user_data);
    
    auto it = self->streams_.find(stream_id);
    if (it != self->streams_.end()) {
        if (it->second->file_fd != -1) {
            close(it->second->file_fd);
        }
        self->streams_.erase(it);
    }
    return 0;
}

ssize_t Http2Session::send_callback(nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data) {
    (void)session;
    (void)flags;
    auto* self = static_cast<Http2Session*>(user_data);
    auto conn = self->connection_.lock();
    if (!conn) {
        // The client connection is gone; nothing can be delivered.
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    std::vector<char> buf(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + length);
    conn->write_raw(buf);
    return static_cast<ssize_t>(length);
}

void Http2Session::dispatch_request(std::shared_ptr<StreamContext> stream_ctx) {
    stream_ctx->request.body = stream_ctx->backing_body;
    for (const auto& pair : stream_ctx->backing_headers) {
        stream_ctx->request.headers[pair.first] = pair.second;
    }
    
    stream_ctx->request.client_ip = client_ip_;
    auto writer = std::make_shared<Http2ResponseWriter>(weak_from_this(), stream_ctx->stream_id);
    
    // The task owns the session and stream context, so both outlive the
    // handler even if the client disconnects meanwhile.
    thread_pool_.enqueue([self = shared_from_this(), stream_ctx, writer]() mutable {
        self->router_.route(stream_ctx->request, writer);
    });
}

ssize_t Http2Session::data_provider_read(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length, uint32_t *data_flags, nghttp2_data_source *source, void *user_data) {
    (void)session;
    (void)stream_id;
    (void)user_data;
    auto* stream_ctx = static_cast<StreamContext*>(source->ptr);
    if (!stream_ctx) return NGHTTP2_ERR_DEFERRED;

    if (stream_ctx->file_fd != -1) {
        size_t to_read = (std::min)(length, static_cast<size_t>(stream_ctx->file_size - stream_ctx->file_offset));
        if (to_read == 0) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        ssize_t bytes = pread(stream_ctx->file_fd, buf, to_read, stream_ctx->file_offset);
        if (bytes < 0) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        stream_ctx->file_offset += bytes;
        if (stream_ctx->file_offset >= stream_ctx->file_size) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return bytes;
    } else {
        size_t to_read = (std::min)(length, stream_ctx->response_body.size() - stream_ctx->response_offset);
        if (to_read == 0) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        std::memcpy(buf, stream_ctx->response_body.data() + stream_ctx->response_offset, to_read);
        stream_ctx->response_offset += to_read;
        if (stream_ctx->response_offset >= stream_ctx->response_body.size()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return static_cast<ssize_t>(to_read);
    }
}

void Http2Session::submit_status_locked(int32_t stream_id, http::HttpStatus status) {
    http::HttpResponse res;
    res.status_code = status;
    detail::HeaderBlock headers = detail::build_response_headers(res);
    nghttp2_submit_response(session_, stream_id, headers.nvs.data(), headers.nvs.size(), nullptr);
}

void Http2Session::submit_response(int32_t stream_id, http::HttpResponse& response, bool has_body) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return;
    auto ctx = it->second;

    // The header block owns the encoded names and values; it must stay alive
    // until nghttp2_submit_response has copied them out.
    detail::HeaderBlock headers = detail::build_response_headers(response);
    std::vector<nghttp2_nv>& nvs = headers.nvs;

    if (has_body) {
        nghttp2_data_provider provider;
        provider.source.ptr = ctx.get();
        provider.read_callback = data_provider_read;
        
        ctx->response_body = response.body;
        ctx->response_offset = 0;
        // Take ownership: the response's destructor would otherwise close the
        // descriptor while this stream is still reading from it.
        ctx->file_fd = response.file_fd;
        response.file_fd = -1;
        ctx->file_size = response.file_size;
        ctx->file_offset = 0;
        
        nghttp2_submit_response(session_, stream_id, nvs.data(), nvs.size(), &provider);
    } else {
        nghttp2_submit_response(session_, stream_id, nvs.data(), nvs.size(), nullptr);
    }
    
    send_pending();
}

void Http2Session::submit_data(int32_t stream_id) {
    (void)stream_id;
    // For streaming chunks, this requires deferred resuming. Not fully implemented yet.
}

void Http2Session::end_stream(int32_t stream_id) {
    (void)stream_id;
    // For streaming chunks end.
}

// ---------------- Http2ResponseWriter ----------------

Http2ResponseWriter::Http2ResponseWriter(std::weak_ptr<Http2Session> session, int32_t stream_id)
    : session_(std::move(session)), stream_id_(stream_id) {}

std::shared_ptr<Http2Session> Http2ResponseWriter::session_or_throw() {
    auto session = session_.lock();
    if (!session) throw std::runtime_error("HTTP/2 session has been closed");
    return session;
}

void Http2ResponseWriter::add_interceptor(Interceptor interceptor) {
    interceptors_.push_back(std::move(interceptor));
}

void Http2ResponseWriter::set_header(const std::string& key, const std::string& value) {
    default_headers_[key] = value;
}

network::Proactor& Http2ResponseWriter::proactor() {
    return session_or_throw()->proactor();
}

concurrency::ThreadPool& Http2ResponseWriter::thread_pool() {
    return session_or_throw()->thread_pool();
}

void Http2ResponseWriter::send(http::HttpResponse&& response) {
    if (headers_sent_) return;
    headers_sent_ = true;
    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }
    bool has_body = !response.body.empty() || response.file_fd != -1;
    if (auto session = session_.lock()) {
        session->submit_response(stream_id_, response, has_body);
    }
}

void Http2ResponseWriter::send_headers(http::HttpResponse& response) {
    if (headers_sent_) return;
    headers_sent_ = true;
    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }
    if (auto session = session_.lock()) {
        session->submit_response(stream_id_, response, true);
    }
}

void Http2ResponseWriter::write_chunk(std::string_view chunk) {
    (void)chunk;
    // Chunked not natively supported yet in this basic HTTP/2 implementation.
    // HTTP/2 DATA frames serve this purpose.
}

void Http2ResponseWriter::end() {
    if (auto session = session_.lock()) {
        session->end_stream(stream_id_);
    }
}

void Http2ResponseWriter::send_sse_event(std::string_view data, std::string_view event, std::string_view id) {
    (void)data;
    (void)event;
    (void)id;
    // Not implemented yet
}

void Http2ResponseWriter::upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) {
    // Not supported in HTTP/2
}

void Http2ResponseWriter::read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) {
    // TODO: HTTP/2 body streaming
}

} // namespace h2
} // namespace http
