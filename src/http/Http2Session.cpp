#include <orbit/http/Http2Session.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/http/HttpParser.hpp>
#include <orbit/utils/FileIO.hpp>
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

bool apply_request_target(std::string_view target, http::HttpRequest& req) {
    req.target = std::string(target);
    size_t q = target.find('?');
    if (!http::percent_decode(target.substr(0, q), req.uri, false, true)) return false;
    return q == std::string_view::npos || http::parse_urlencoded(target.substr(q + 1), req.query);
}

void parse_cookies(std::string_view cookies, http::HttpRequest& req) {
    size_t pos = 0;
    while (pos < cookies.size()) {
        while (pos < cookies.size() && cookies[pos] == ' ') ++pos;
        size_t semi = cookies.find(';', pos);
        std::string_view pair = cookies.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        size_t eq = pair.find('=');
        if (eq != std::string_view::npos) {
            req.cookies[std::string(pair.substr(0, eq))] = std::string(pair.substr(eq + 1));
        }
        if (semi == std::string_view::npos) break;
        pos = semi + 1;
    }
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
            utils::file::close(ctx->file_fd);
            ctx->file_fd = -1;
        }
    }
}

void Http2Session::begin_shutdown() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(session_),
                          NGHTTP2_NO_ERROR, nullptr, 0);
    send_pending();
}

bool Http2Session::idle() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return streams_.empty();
}

void Http2Session::process_data(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    ssize_t rv = nghttp2_session_mem_recv(session_, data, len);
    if (rv < 0) {
        // A fatal error nghttp2 did not already answer: tell the peer why
        // (GOAWAY) before closing; send_pending() then closes.
        nghttp2_session_terminate_session(session_, NGHTTP2_PROTOCOL_ERROR);
    }
    send_pending();
}

void Http2Session::send_pending() {
    nghttp2_session_send(session_);
    // When nghttp2 wants neither to read nor to write, the session is over
    // (a GOAWAY was sent or received and every stream has finished). Close
    // once what was queued, including our GOAWAY, has been written; leaving
    // the connection open made peers wait for a close that never came.
    if (!nghttp2_session_want_read(session_) && !nghttp2_session_want_write(session_)) {
        if (auto conn = connection_.lock()) conn->mark_for_close();
    }
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
        req.target = val;
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
    
    // END_STREAM can arrive on the request HEADERS, the last DATA frame, or
    // a trailing HEADERS frame (trailers, e.g. after a gRPC-style body).
    bool ends_request = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        (frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA);
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
            utils::file::close(it->second->file_fd);
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
    auto& req = stream_ctx->request;
    req.body = stream_ctx->backing_body;

    // RFC 9113 section 8.2.3: the Cookie field may arrive split across
    // several "cookie" fields; join them before anyone reads it.
    for (const auto& pair : stream_ctx->backing_headers) {
        if (pair.first == "cookie") {
            if (!stream_ctx->backing_cookie.empty()) stream_ctx->backing_cookie += "; ";
            stream_ctx->backing_cookie += pair.second;
        } else {
            req.headers[pair.first] = pair.second;
        }
    }
    if (!stream_ctx->backing_cookie.empty()) {
        req.headers["cookie"] = stream_ctx->backing_cookie;
        detail::parse_cookies(stream_ctx->backing_cookie, req);
    }

    // :path carries the query string. Route on the decoded path and expose a
    // decoded query, exactly as the HTTP/1.1 parser does.
    if (!detail::apply_request_target(stream_ctx->backing_uri, req)) {
        // Malformed escape, or an encoded '/', '\\' or NUL in the path: 400,
        // as for HTTP/1.1. Called from nghttp2 callbacks, so the lock is held.
        submit_status_locked(stream_ctx->stream_id, http::HttpStatus::BadRequest);
        return;
    }
    
    req.client_ip = client_ip_;
    req.peer_ip = client_ip_;
    auto writer = std::make_shared<Http2ResponseWriter>(weak_from_this(), stream_ctx->stream_id,
                                                        req.method == http::HttpMethod::HEAD);
    writer->body_owner_ = stream_ctx;
    writer->body_ = stream_ctx->backing_body;
    
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
        ssize_t bytes = utils::file::pread(stream_ctx->file_fd, buf, to_read, stream_ctx->file_offset);
        if (bytes < 0) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        stream_ctx->file_offset += bytes;
        if (stream_ctx->file_offset >= stream_ctx->file_size) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }
        return bytes;
    } else {
        const bool more_to_come = stream_ctx->streaming && !stream_ctx->stream_ended;
        size_t to_read = (std::min)(length, stream_ctx->response_body.size() - stream_ctx->response_offset);
        if (to_read == 0) {
            if (more_to_come) {
                return NGHTTP2_ERR_DEFERRED; // resumed by submit_data()/end_stream()
            }
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        std::memcpy(buf, stream_ctx->response_body.data() + stream_ctx->response_offset, to_read);
        stream_ctx->response_offset += to_read;
        if (stream_ctx->response_offset >= stream_ctx->response_body.size()) {
            if (more_to_come) {
                // Everything written so far is sent; drop it.
                stream_ctx->response_body.clear();
                stream_ctx->response_offset = 0;
            } else {
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            }
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

void Http2Session::submit_response(int32_t stream_id, http::HttpResponse& response, bool has_body, bool streaming) {
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
        ctx->streaming = streaming;
        // Take ownership: the response's destructor would otherwise close the
        // descriptor while this stream is still reading from it.
        ctx->file_fd = response.file_fd;
        response.file_fd = -1;
        ctx->file_size = response.file_size;
        ctx->file_offset = response.file_offset;
        
        nghttp2_submit_response(session_, stream_id, nvs.data(), nvs.size(), &provider);
    } else {
        nghttp2_submit_response(session_, stream_id, nvs.data(), nvs.size(), nullptr);
    }
    
    send_pending();
}

void Http2Session::submit_data(int32_t stream_id, std::string_view chunk) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second->streaming || it->second->stream_ended) return;
    it->second->response_body.append(chunk);
    nghttp2_session_resume_data(session_, stream_id);
    send_pending();
}

void Http2Session::end_stream(int32_t stream_id) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second->streaming || it->second->stream_ended) return;
    it->second->stream_ended = true;
    nghttp2_session_resume_data(session_, stream_id);
    send_pending();
}

// ---------------- Http2ResponseWriter ----------------

Http2ResponseWriter::Http2ResponseWriter(std::weak_ptr<Http2Session> session, int32_t stream_id, bool suppress_body)
    : session_(std::move(session)), stream_id_(stream_id), suppress_body_(suppress_body) {}

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

void Http2ResponseWriter::apply_response_hooks(http::HttpResponse& response) {
    // Same order as the HTTP/1.1 connection: interceptors, then defaults.
    for (auto& interceptor : interceptors_) {
        interceptor(response);
    }
    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }
}

void Http2ResponseWriter::send(http::HttpResponse&& response) {
    mark_responded();
    if (headers_sent_) return;
    headers_sent_ = true;
    apply_response_hooks(response);
    // HEAD, 1xx, 204 and 304 responses carry headers only.
    int code = static_cast<int>(response.status_code);
    bool bodiless = suppress_body_ || code < 200 || code == 204 || code == 304;
    bool has_body = !bodiless && (!response.body.empty() || response.file_fd != -1);
    if (auto session = session_.lock()) {
        session->submit_response(stream_id_, response, has_body);
    }
}

void Http2ResponseWriter::send_headers(http::HttpResponse& response) {
    mark_responded();
    if (headers_sent_) return;
    headers_sent_ = true;
    apply_response_hooks(response);
    if (auto session = session_.lock()) {
        // The body follows through write_chunk() and ends with end(); for
        // HEAD the headers end the stream and later chunks are dropped.
        session->submit_response(stream_id_, response, !suppress_body_, !suppress_body_);
    }
}

void Http2ResponseWriter::write_chunk(std::string_view chunk) {
    mark_responded();
    if (chunk.empty()) return;
    if (auto session = session_.lock()) {
        session->submit_data(stream_id_, chunk);
    }
}

void Http2ResponseWriter::end() {
    mark_responded();
    if (auto session = session_.lock()) {
        session->end_stream(stream_id_);
    }
}

void Http2ResponseWriter::send_sse_event(std::string_view data, std::string_view event, std::string_view id) {
    mark_responded();
    std::string msg;
    if (!event.empty()) msg += "event: " + std::string(event) + "\n";
    if (!id.empty()) msg += "id: " + std::string(id) + "\n";
    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        msg += "data: " + std::string(data.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start)) + "\n";
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    msg += "\n";
    write_chunk(msg);
}

void Http2ResponseWriter::upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) {
    (void)on_data;
    (void)on_close;
    // HTTP/2 has no connection-level upgrade (that needs RFC 8441 extended CONNECT).
    LOG_WARN("upgrade_to_raw_stream is not supported on HTTP/2 connections");
}

void Http2ResponseWriter::read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) {
    // HTTP/2 requests are dispatched once the whole body has arrived, so the
    // "stream" is the buffered body delivered at once.
    if (!body_.empty() && on_data) on_data(body_);
    if (on_end) on_end();
}

} // namespace h2
} // namespace http
