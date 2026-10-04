#include <orbit/server/QuicHttp3Session.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/utils/FileIO.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/Http2Session.hpp>

#include <orbit/server/QuicConnection.hpp>
#include <orbit/server/QuicConnectionManager.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#undef DELETE
#undef ERROR
#endif

namespace server {

namespace {

constexpr size_t kFileChunk = 64 * 1024;

/// Owns the strings an nghttp3_nv list points into.
struct Http3HeaderBlock {
    std::vector<std::string> storage;
    std::vector<nghttp3_nv> nvs;
};

Http3HeaderBlock build_response_headers(const http::HttpResponse& response) {
    Http3HeaderBlock block;
    // Reserved up front: the nv entries point into these strings.
    block.storage.reserve(2 + response.headers.size() * 2); // name and value each
    block.nvs.reserve(1 + response.headers.size());
    auto add = [&block](std::string name, std::string value) {
        block.storage.push_back(std::move(name));
        const std::string& n = block.storage.back();
        block.storage.push_back(std::move(value));
        const std::string& v = block.storage.back();
        block.nvs.push_back({reinterpret_cast<uint8_t*>(const_cast<char*>(n.data())),
                             reinterpret_cast<uint8_t*>(const_cast<char*>(v.data())),
                             n.size(), v.size(), NGHTTP3_NV_FLAG_NONE});
    };
    add(":status", std::to_string(static_cast<int>(response.status_code)));
    for (const auto& [k, v] : response.headers) {
        // RFC 9114 section 4.2: connection-specific fields are malformed in HTTP/3.
        if (http::h2::detail::is_connection_specific_header(k)) continue;
        std::string name;
        name.reserve(k.size());
        for (char c : k) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        add(std::move(name), v);
    }
    return block;
}

} // namespace

Http3Stream::~Http3Stream() {
    if (file_fd != -1) utils::file::close(file_fd);
}

QuicHttp3Session::QuicHttp3Session(QuicConnection& quic_conn) : quic_conn_(quic_conn) {
}

QuicHttp3Session::~QuicHttp3Session() {
    if (httpconn_) {
        nghttp3_conn_del(httpconn_);
    }
}

bool QuicHttp3Session::init() {
    nghttp3_callbacks callbacks{};
    callbacks.acked_stream_data = on_acked_stream_data;
    callbacks.stream_close = on_stream_close;
    callbacks.recv_data = on_recv_data;
    callbacks.deferred_consume = on_deferred_consume;
    callbacks.begin_headers = on_begin_headers;
    callbacks.recv_header = on_recv_header;
    callbacks.end_headers = on_end_headers;
    callbacks.end_stream = on_end_stream;

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);

    int rv = nghttp3_conn_server_new(&httpconn_, &callbacks, &settings, nullptr, this);
    if (rv != 0) {
        LOG_ERROR("nghttp3_conn_server_new failed: " << nghttp3_strerror(rv));
        return false;
    }
    return true;
}

nghttp3_ssize QuicHttp3Session::process_stream_data(int64_t stream_id, const uint8_t* data, size_t datalen, bool fin) {
    if (!httpconn_) return 0;

    nghttp3_ssize consumed = nghttp3_conn_read_stream(httpconn_, stream_id, data, datalen, fin);
    if (consumed < 0) {
        LOG_ERROR("nghttp3_conn_read_stream failed: rv=" << consumed << " msg=" << nghttp3_strerror(static_cast<int>(consumed)));
    }
    return consumed;
}

std::shared_ptr<Http3Stream> QuicHttp3Session::get_or_create_stream(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it != streams_.end()) {
        return it->second;
    }
    auto stream = std::make_shared<Http3Stream>(stream_id);
    streams_[stream_id] = stream;
    return stream;
}

int QuicHttp3Session::on_acked_stream_data(nghttp3_conn *, int64_t, uint64_t datalen, void *, void *stream_user_data) {
    auto* s = static_cast<Http3Stream*>(stream_user_data);
    if (!s) return 0;
    // ACKs cover the body bytes in the order read_data handed them out.
    s->front_acked += datalen;
    while (s->handed_out > 0 && s->front_acked >= s->chunks.front().size()) {
        s->front_acked -= s->chunks.front().size();
        s->chunks.pop_front();
        --s->handed_out;
    }
    return 0;
}

int QuicHttp3Session::on_stream_close(nghttp3_conn * /*conn*/, int64_t stream_id, uint64_t /*app_error_code*/, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    session->streams_.erase(stream_id);
    return 0;
}

int QuicHttp3Session::on_recv_data(nghttp3_conn * /*conn*/, int64_t stream_id, const uint8_t *data, size_t datalen, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    size_t limit = session->quic_conn_.manager().http_context().max_body_size;
    if (!stream->body_too_large) {
        if (limit != 0 && stream->body.size() + datalen > limit) {
            // Stop buffering; the request is answered with 413 when it ends.
            stream->body_too_large = true;
            std::string().swap(stream->body);
        } else {
            stream->body.append(reinterpret_cast<const char*>(data), datalen);
        }
    }
    // read_stream's return value excludes DATA payload delivered here; the
    // application returns that credit once it has taken the bytes.
    session->quic_conn_.extend_stream_credit(stream_id, datalen);
    return 0;
}

int QuicHttp3Session::on_end_stream(nghttp3_conn *, int64_t stream_id, void *conn_user_data, void *) {
    // The request (with its body) is complete.
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    if (stream->headers_complete && !stream->dispatched) session->handle_request(stream);
    return 0;
}

int QuicHttp3Session::on_deferred_consume(nghttp3_conn *, int64_t stream_id, size_t consumed, void *conn_user_data, void *) {
    // Data nghttp3 held back earlier has now been processed: return its credit.
    static_cast<QuicHttp3Session*>(conn_user_data)->quic_conn_.extend_stream_credit(stream_id, consumed);
    return 0;
}

int QuicHttp3Session::on_begin_headers(nghttp3_conn *conn, int64_t stream_id, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    nghttp3_conn_set_stream_user_data(conn, stream_id, stream.get());
    return 0;
}

int QuicHttp3Session::on_recv_header(nghttp3_conn * /*conn*/, int64_t stream_id, int32_t /*token*/, nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t /*flags*/, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    if (stream->headers_complete) return 0; // trailers are not exposed

    auto name_buf = nghttp3_rcbuf_get_buf(name);
    auto value_buf = nghttp3_rcbuf_get_buf(value);
    std::string_view key(reinterpret_cast<const char*>(name_buf.base), name_buf.len);
    std::string_view val(reinterpret_cast<const char*>(value_buf.base), value_buf.len);

    if (key == ":method") {
        http::h2::detail::parse_method(val, stream->request.method);
    } else if (key == ":path") {
        stream->path = std::string(val);
    } else if (key == ":authority") {
        stream->request.set_header("Host", std::string(val));
    } else if (key == "cookie") {
        // RFC 9114 section 4.2.1: cookies may be split across fields.
        if (!stream->cookie.empty()) stream->cookie += "; ";
        stream->cookie += val;
    } else if (!key.empty() && key[0] != ':') {
        stream->request.set_header(key, std::string(val));
    }
    return 0;
}

int QuicHttp3Session::on_end_headers(nghttp3_conn * /*conn*/, int64_t stream_id, int fin, void *conn_user_data, void * /*stream_user_data*/) {
    auto session = static_cast<QuicHttp3Session*>(conn_user_data);
    auto stream = session->get_or_create_stream(stream_id);
    stream->headers_complete = true;
    if (fin && !stream->dispatched) {
        session->handle_request(stream);
    }
    return 0;
}

void QuicHttp3Session::handle_request(std::shared_ptr<Http3Stream> stream) {
    stream->dispatched = true;
    const Http3Context& ctx = quic_conn_.manager().http_context();
    if (!ctx.router || !ctx.thread_pool || !ctx.proactor) {
        submit_status(stream->stream_id, http::HttpStatus::ServiceUnavailable);
        return;
    }
    if (stream->body_too_large) {
        submit_status(stream->stream_id, http::HttpStatus::PayloadTooLarge);
        return;
    }

    auto& req = stream->request;
    req.http_version = "HTTP/3";
    req.body = stream->body;
    if (!stream->cookie.empty()) {
        req.set_header("cookie", stream->cookie);
        http::h2::detail::parse_cookies(stream->cookie, req);
    }
    if (!http::h2::detail::apply_request_target(stream->path, req)) {
        submit_status(stream->stream_id, http::HttpStatus::BadRequest);
        return;
    }
    req.client_ip = quic_conn_.remote_ip();
    req.peer_ip = req.client_ip;

    auto writer = std::make_shared<Http3ResponseWriter>(quic_conn_.weak_from_this(), stream->stream_id,
                                                        *ctx.proactor, *ctx.thread_pool,
                                                        req.method == http::HttpMethod::HEAD);
    writer->body_owner_ = stream;
    writer->body_ = stream->body;

    // Handlers may block; run them off the event loop. The task owns the
    // stream, so the request outlives the handler even if the client leaves.
    const routing::Router* router = ctx.router;
    ctx.thread_pool->enqueue([router, stream, writer]() {
        router->route(stream->request, writer);
    });
}

nghttp3_ssize QuicHttp3Session::read_data(nghttp3_conn *, int64_t, nghttp3_vec *vec, size_t veccnt, uint32_t *pflags, void *, void *stream_user_data) {
    auto* s = static_cast<Http3Stream*>(stream_user_data);
    if (!s) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    // File bodies are read a piece at a time; ACKed pieces are freed.
    if (s->file_fd != -1 && s->handed_out == s->chunks.size() && s->file_offset < s->file_end) {
        size_t want = static_cast<size_t>((std::min<int64_t>)(static_cast<int64_t>(kFileChunk), s->file_end - s->file_offset));
        std::string piece(want, '\0');
        long long n = utils::file::pread(s->file_fd, piece.data(), want, s->file_offset);
        if (n <= 0) return NGHTTP3_ERR_CALLBACK_FAILURE; // the file shrank or failed: reset the stream
        piece.resize(static_cast<size_t>(n));
        s->file_offset += n;
        s->chunks.push_back(std::move(piece));
    }

    size_t n = 0;
    while (n < veccnt && s->handed_out < s->chunks.size()) {
        std::string& chunk = s->chunks[s->handed_out];
        vec[n].base = reinterpret_cast<uint8_t*>(chunk.data());
        vec[n].len = chunk.size();
        ++s->handed_out;
        ++n;
    }

    bool more = s->handed_out < s->chunks.size() ||
                (s->file_fd != -1 && s->file_offset < s->file_end) ||
                (s->streaming && !s->ended);
    if (!more) {
        *pflags |= NGHTTP3_DATA_FLAG_EOF;
    } else if (n == 0) {
        return NGHTTP3_ERR_WOULDBLOCK; // resumed by submit_data()/end_stream()
    }
    return static_cast<nghttp3_ssize>(n);
}

void QuicHttp3Session::submit_status(int64_t stream_id, http::HttpStatus status) {
    http::HttpResponse res;
    res.status_code = status;
    res.headers["Content-Length"] = "0";
    submit_response(stream_id, res, false);
}

void QuicHttp3Session::submit_response(int64_t stream_id, http::HttpResponse& response, bool has_body, bool streaming) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return; // the stream was reset or closed meanwhile
    Http3Stream& s = *it->second;

    Http3HeaderBlock headers = build_response_headers(response);
    nghttp3_data_reader reader{read_data};
    if (has_body) {
        s.streaming = streaming;
        // A streamed response's headers object stays the caller's: copy.
        if (!response.body.empty()) {
            if (streaming) s.chunks.push_back(response.body);
            else s.chunks.push_back(std::move(response.body));
        }
        if (response.file_fd != -1) {
            // Take ownership: the response would otherwise close it while
            // the stream is still reading.
            s.file_fd = response.file_fd;
            response.file_fd = -1;
            s.file_offset = static_cast<int64_t>(response.file_offset);
            s.file_end = static_cast<int64_t>(response.file_size);
        }
    }
    int rv = nghttp3_conn_submit_response(httpconn_, stream_id, headers.nvs.data(), headers.nvs.size(),
                                          has_body ? &reader : nullptr);
    if (rv != 0) {
        LOG_ERROR("nghttp3_conn_submit_response failed: " << nghttp3_strerror(rv));
    }
}

void QuicHttp3Session::submit_data(int64_t stream_id, std::string_view chunk) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second->streaming || it->second->ended || chunk.empty()) return;
    it->second->chunks.emplace_back(chunk);
    nghttp3_conn_resume_stream(httpconn_, stream_id);
}

void QuicHttp3Session::end_stream(int64_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second->streaming || it->second->ended) return;
    it->second->ended = true;
    nghttp3_conn_resume_stream(httpconn_, stream_id);
}

// ---------------- Http3ResponseWriter ----------------

Http3ResponseWriter::Http3ResponseWriter(std::weak_ptr<QuicConnection> conn, int64_t stream_id,
                                         network::Proactor& proactor, concurrency::ThreadPool& thread_pool,
                                         bool suppress_body)
    : conn_(std::move(conn)), stream_id_(stream_id), proactor_(proactor), thread_pool_(thread_pool),
      suppress_body_(suppress_body) {}

template <typename Fn>
void Http3ResponseWriter::with_session(Fn&& fn) {
    auto conn = conn_.lock();
    if (!conn) return; // the connection is gone
    std::lock_guard<std::recursive_mutex> lock(conn->mutex());
    QuicHttp3Session* session = conn->http3_session();
    if (!session || conn->is_closed()) return;
    fn(*session);
    conn->send_pending_data();
}

void Http3ResponseWriter::add_interceptor(Interceptor interceptor) {
    interceptors_.push_back(std::move(interceptor));
}

void Http3ResponseWriter::set_header(const std::string& key, const std::string& value) {
    default_headers_[key] = value;
}

void Http3ResponseWriter::apply_response_hooks(http::HttpResponse& response) {
    // Same order as HTTP/1.1 and HTTP/2: interceptors, then defaults.
    for (auto& interceptor : interceptors_) {
        interceptor(response);
    }
    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }
}

void Http3ResponseWriter::send(http::HttpResponse&& response) {
    if (headers_sent_) return;
    headers_sent_ = true;
    apply_response_hooks(response);
    // HEAD, 1xx, 204 and 304 responses carry headers only.
    int code = static_cast<int>(response.status_code);
    bool bodiless = suppress_body_ || code < 200 || code == 204 || code == 304;
    bool has_body = !bodiless && (!response.body.empty() || response.file_fd != -1);
    with_session([&](QuicHttp3Session& s) { s.submit_response(stream_id_, response, has_body); });
}

void Http3ResponseWriter::send_headers(http::HttpResponse& response) {
    if (headers_sent_) return;
    headers_sent_ = true;
    apply_response_hooks(response);
    // The body follows through write_chunk() and ends with end(); for HEAD
    // the headers end the stream and later chunks are dropped.
    with_session([&](QuicHttp3Session& s) {
        s.submit_response(stream_id_, response, !suppress_body_, !suppress_body_);
    });
}

void Http3ResponseWriter::write_chunk(std::string_view chunk) {
    if (chunk.empty()) return;
    with_session([&](QuicHttp3Session& s) { s.submit_data(stream_id_, chunk); });
}

void Http3ResponseWriter::end() {
    with_session([&](QuicHttp3Session& s) { s.end_stream(stream_id_); });
}

void Http3ResponseWriter::send_sse_event(std::string_view data, std::string_view event, std::string_view id) {
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

void Http3ResponseWriter::upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) {
    (void)on_data;
    (void)on_close;
    // HTTP/3 has no connection-level upgrade (that needs RFC 9220 extended CONNECT).
    LOG_WARN("upgrade_to_raw_stream is not supported on HTTP/3 connections");
}

void Http3ResponseWriter::read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) {
    // HTTP/3 requests are dispatched once the whole body has arrived, so the
    // "stream" is the buffered body delivered at once.
    if (!body_.empty() && on_data) on_data(body_);
    if (on_end) on_end();
}

} // namespace server
