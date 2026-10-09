#include <orbit/server/Connection.hpp>
#include <limits>
#include <algorithm>
#include <orbit/utils/Logger.hpp>
#include <orbit/server/ConnectionManager.hpp>
#include <orbit/http/HttpParser.hpp>
#include <orbit/http/WebSocket.hpp>
#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/http/Http2Session.hpp>
#include <orbit/config/Config.hpp>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <iostream>
#include <orbit/network/PlatformSocket.hpp>
#include <orbit/utils/FileIO.hpp>


namespace orbit::server {

Connection::Connection(network::Socket socket, const std::string& client_ip, network::Proactor& proactor, const routing::Router& router, ConnectionManager& manager, concurrency::ThreadPool& thread_pool, TimerManager& timer_manager, size_t max_body_size, network::TlsContext* tls_context)
    : socket_(std::move(socket)), client_ip_(client_ip), proactor_(proactor), router_(router), manager_(manager), thread_pool_(thread_pool), timer_manager_(timer_manager), max_body_size_(max_body_size) {
    
    if (tls_context) {
        ssl_ = SSL_new(tls_context->get());
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl_, rbio_, wbio_);
        SSL_set_accept_state(ssl_); // Set to server mode
    }
}

Connection::~Connection() {
    mark_closed(); // usually done already, by on_removed()
    if (current_timer_id_ != 0) {
        timer_manager_.cancel_timer(current_timer_id_);
    }
    // Whatever tore the connection down (peer close, timeout, I/O error),
    // WebSocket users must hear about it before the object they hold a
    // reference to is destroyed.
    if (ws_connection_) {
        ws_connection_->handle_transport_closed();
    }
    if (ssl_) {
        SSL_free(ssl_);
    }
    if (file_fd_ != -1) {
        utils::file::close(file_fd_);
    }
}

void Connection::arm_timer(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (current_timer_id_ != 0) {
        timer_manager_.cancel_timer(current_timer_id_);
        current_timer_id_ = 0;
    }
    if (timeout.count() > 0) {
        current_timer_id_ = timer_manager_.add_timer(socket_.fd(), timeout);
    }
}

// Picks the timeout for whatever the connection is waiting on right now.
void Connection::arm_timer_for_current_phase() {
    switch (state_) {
        case ConnectionState::WEBSOCKET:
        case ConnectionState::RAW_STREAM:
            arm_timer(timeouts_.websocket_idle);
            return;
        case ConnectionState::HTTP2:
        case ConnectionState::HTTP_STREAMING_BODY:
            arm_timer(timeouts_.idle);
            return;
        default:
            break;
    }
    if (is_processing_request_) {
        // A handler is running (possibly a long computation, a slow database
        // call or an SSE stream). Its duration is the application's business.
        arm_timer(std::chrono::milliseconds(0));
    } else {
        arm_timer(timeouts_.keep_alive);
    }
}

void Connection::start() {
    arm_timer(timeouts_.header);
    trigger_read();
}

void Connection::trigger_read() {
    bool expected = false;
    if (is_reading_.compare_exchange_strong(expected, true)) {
        auto self = shared_from_this();
        bool submitted = submit_io([&] {
            proactor_.async_read(socket_.fd(), async_read_buf_, sizeof(async_read_buf_), [self](ssize_t bytes) {
                self->is_reading_ = false;
                self->on_read_complete(bytes);
            });
        });
        if (!submitted) is_reading_ = false;
    }
}

void Connection::on_removed(bool notify_now) {
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        removed_ = true;
    }
    // Writers held past this point (SSE subscribers, chunked streams) learn
    // that the client is gone (#197). Normally on a pool thread: a removal
    // can come from inside this connection's or its HTTP/2 session's locked
    // processing (an HTTP/2 session closing itself after GOAWAY does), and
    // an on_close callback may write to the writer, which takes those locks.
    auto notify = [](const std::shared_ptr<Connection>& self) {
        if (self->h2_session_) self->h2_session_->close_streams();
        self->mark_closed();
    };
    if (notify_now) {
        notify(shared_from_this());
    } else {
        thread_pool_.enqueue([self = shared_from_this(), notify] { notify(self); });
    }
}

namespace {

// True if a proactor I/O result means "not ready yet" rather than failure.
// epoll and kqueue report -1 with errno; io_uring reports -errno. IOCP
// completions never mean would-block.
bool would_block(ssize_t result) {
#ifdef _WIN32
    (void)result;
    return false;
#else
    if (result == -EAGAIN || result == -EWOULDBLOCK || result == -EINTR) return true;
    return result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
#endif
}

} // namespace

void Connection::on_read_complete(ssize_t bytes_read) {
    if (bytes_read < 0 && would_block(bytes_read)) {
        // Spurious wakeup: nothing to read yet. Not a closed connection.
        trigger_read();
        return;
    }
    if (bytes_read <= 0) {
        if (state_ == ConnectionState::RAW_STREAM && raw_stream_on_close_) {
            raw_stream_on_close_();
        }
        if (state_ == ConnectionState::HTTP_STREAMING_BODY && body_stream_on_end_) {
            body_stream_on_end_();
        }
        if (state_ == ConnectionState::WEBSOCKET && ws_connection_) {
            ws_connection_->handle_transport_closed();
        }
        LOG_DEBUG("on_read_complete closed with bytes_read=" << bytes_read);
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    bool buffer_was_empty;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        // The parser drains read_buffer_ as it goes, so an empty buffer alone
        // does not mean a new request: a half-parsed one may be in progress.
        buffer_was_empty = read_buffer_.empty() && !message_started_;
    }
    
    bool have_tls_output = false;
    if (ssl_) {
        std::vector<char> plaintext;
        bool handshake_failed = false;
        bool handshake_pending = false;
        bool negotiated_h2 = false;
        bool tls_closed = false; // peer sent close_notify, or the TLS stream failed
        {
            std::lock_guard<std::mutex> tls_lock(tls_mutex_);
            BIO_write(rbio_, async_read_buf_, static_cast<int>(bytes_read));
            
            if (!is_tls_handshake_complete_) {
                int ret = SSL_do_handshake(ssl_);
                if (ret == 1) {
                    is_tls_handshake_complete_ = true;
                    const unsigned char* alpn = nullptr;
                    unsigned int alpn_len = 0;
                    SSL_get0_alpn_selected(ssl_, &alpn, &alpn_len);
                    negotiated_h2 = (alpn_len == 2 && std::memcmp(alpn, "h2", 2) == 0);
                } else {
                    int err = SSL_get_error(ssl_, ret);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                        handshake_pending = true;
                    } else {
                        handshake_failed = true;
                    }
                }
            }
            
            if (is_tls_handshake_complete_) {
                char clear_buf[8192];
                while (true) {
                    int ret = SSL_read(ssl_, clear_buf, sizeof(clear_buf));
                    if (ret <= 0) {
                        int err = SSL_get_error(ssl_, ret);
                        // WANT_READ: the record is incomplete, wait for more bytes.
                        // ZERO_RETURN (close_notify) or any other error: this TLS
                        // stream will not deliver more data.
                        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                            tls_closed = true;
                        }
                        break;
                    }
                    plaintext.insert(plaintext.end(), clear_buf, clear_buf + ret);
                }
            }

            drain_tls_output_locked();
            have_tls_output = !tls_write_buffer_.empty();
        }

        if (handshake_failed) {
            manager_.remove_connection(socket_.fd());
            return;
        }
        if (tls_closed) {
            if (plaintext.empty() && !is_processing_request_) {
                // Nothing left to answer: close as for a TCP FIN.
                on_read_complete(0);
                return;
            }
            // Answer what was already received, then close.
            should_close_ = true;
        }
        if (handshake_pending) {
            if (have_tls_output) {
                trigger_write();
            }
            trigger_read(); // Continue reading handshake data
            return;
        }
        if (negotiated_h2) {
            state_ = ConnectionState::HTTP2;
            h2_session_ = std::make_shared<http::h2::Http2Session>(
                std::weak_ptr<Connection>(shared_from_this()), proactor_, router_, thread_pool_,
                client_ip_, max_body_size_);
        }
        if (!plaintext.empty()) {
            std::lock_guard<std::mutex> lock(read_mutex_);
            read_buffer_.insert(read_buffer_.end(), plaintext.begin(), plaintext.end());
        }
    } else {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_buffer_.insert(read_buffer_.end(), async_read_buf_, async_read_buf_ + bytes_read);
    }
    
    // h2c with prior knowledge (RFC 9113 section 3.3): a plaintext client
    // may open with the HTTP/2 preface instead of an HTTP/1.1 request.
    if (!ssl_ && limits_.h2c && state_ == ConnectionState::HTTP && !h2c_decided_) {
        static constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        bool is_preface = false;
        bool maybe_preface = false;
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            std::string_view head(read_buffer_.data(), std::min(read_buffer_.size(), kPreface.size()));
            is_preface = head.size() == kPreface.size() && head == kPreface;
            maybe_preface = head.size() < kPreface.size() && kPreface.substr(0, head.size()) == head;
        }
        if (maybe_preface) {
            // Not enough bytes to tell yet; an HTTP/1.1 parse of a partial
            // preface would see a "PRI" request. The header timeout applies.
            if (buffer_was_empty) arm_timer(timeouts_.header);
            trigger_read();
            return;
        }
        h2c_decided_ = true; // prior knowledge is only ever at the start
        if (is_preface) {
            state_ = ConnectionState::HTTP2;
            // nghttp2 expects the preface itself, so the bytes stay buffered.
            h2_session_ = std::make_shared<http::h2::Http2Session>(
                std::weak_ptr<Connection>(shared_from_this()), proactor_, router_, thread_pool_,
                client_ip_, max_body_size_);
        }
    }

    // We defer checking max body size to check_request_state()
    
    if (have_tls_output) {
        trigger_write();
    }
    
    if (state_ != ConnectionState::HTTP) {
        arm_timer_for_current_phase();
    }

    if (state_ == ConnectionState::RAW_STREAM) {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (!read_buffer_.empty() && raw_stream_on_data_) {
            raw_stream_on_data_(std::string_view(read_buffer_.data(), read_buffer_.size()));
            read_buffer_.clear();
        }
        trigger_read();
        return;
    }
    
    if (state_ == ConnectionState::HTTP_STREAMING_BODY) {
        process_streaming_data();
        trigger_read();
        return;
    }
    
    if (state_ == ConnectionState::WEBSOCKET) {
        if (ws_connection_) {
            std::lock_guard<std::mutex> lock(read_mutex_);
            ws_connection_->process_raw_data(read_buffer_);
        }
        trigger_read();
        return;
    }
    
    if (state_ == ConnectionState::HTTP2) {
        if (h2_session_) {
            std::vector<char> local_buf;
            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                local_buf = std::move(read_buffer_);
                read_buffer_.clear();
            }
            if (!local_buf.empty()) {
                h2_session_->process_data(reinterpret_cast<const uint8_t*>(local_buf.data()), local_buf.size());
            }
        }
        trigger_read();
        return;
    }
    
    // A handler is using the request the parser holds: keep these bytes
    // buffered until continue_after_response() moves on to them.
    if (is_processing_request_) return;

    RequestState state = check_request_state();

    if (state == RequestState::INCOMPLETE && !is_processing_request_) {
        bool headers_done;
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            headers_done = headers_parsed_;
        }
        if (headers_done) {
            // The body is within limits (oversized ones were refused above):
            // tell a client waiting on "Expect: 100-continue" to send it.
            send_continue_if_expected();
            // Receiving the body: each read extends the deadline.
            arm_timer(timeouts_.idle);
        } else if (buffer_was_empty) {
            // First bytes of a new request: the headers must all arrive within
            // header_timeout. Later bytes do not extend it, so a client cannot
            // hold the connection open by trickling header bytes.
            arm_timer(timeouts_.header);
        }
    }
    
    if (state == RequestState::COMPLETE || state == RequestState::HEADERS_COMPLETE) {
        bool expected = false;
        if (is_processing_request_.compare_exchange_strong(expected, true)) {
            arm_timer(state == RequestState::HEADERS_COMPLETE ? timeouts_.idle : std::chrono::milliseconds(0));
            if (state == RequestState::HEADERS_COMPLETE) {
                state_ = ConnectionState::HTTP_STREAMING_BODY;
                send_continue_if_expected();
            }
            auto self = shared_from_this();
            thread_pool_.enqueue([self]() {
                self->process_request();
            });
            
            // For streaming bodies, we must continue reading asynchronously
            if (state == RequestState::HEADERS_COMPLETE) {
                trigger_read();
            }
        }
    } else if (state == RequestState::ERROR_PAYLOAD_TOO_LARGE) {
        send_error(http::HttpStatus::PayloadTooLarge, "413 Payload Too Large");
    } else if (state == RequestState::ERROR_HEADERS_TOO_LARGE) {
        send_error(http::HttpStatus::RequestHeaderFieldsTooLarge, "431 Request Header Fields Too Large");
    } else if (state == RequestState::ERROR_BAD_REQUEST) {
        send_error(http::HttpStatus::BadRequest, "400 Bad Request");
    } else if (state == RequestState::ERROR_NOT_IMPLEMENTED) {
        send_error(http::HttpStatus::NotImplemented, "501 Not Implemented");
    } else {
        trigger_read();
    }
}

void Connection::process_request() {
    // Any 100 Continue for this request has been sent; the next may need one.
    continue_sent_ = false;
    reset_responded();
    {
        // Headers and interceptors registered by middleware belong to one
        // request. Keeping them would re-apply them to every later response
        // on this keep-alive connection (e.g. gzip applied twice).
        std::lock_guard<std::mutex> lock(write_mutex_);
        default_headers_.clear();
        interceptors_.clear();
    }
    // The parser holds the request (headers and body owned by it) and is not
    // fed again until continue_after_response() calls next().
    http::HttpRequest* parsed_req = nullptr;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (parser_ && (request_complete_ || streaming_request_)) parsed_req = &parser_->request();
    }

    if (parsed_req) {
        http::HttpRequest& req = *parsed_req;
        req.client_ip = client_ip_;
        req.peer_ip = client_ip_;
        is_head_request_ = (req.method == http::HttpMethod::HEAD);
        
        // WebSocket Upgrade Interception
        auto upgrade_it = req.headers.find("Upgrade");
        if (upgrade_it != req.headers.end() &&
            http::connection_option_present(upgrade_it->second, "websocket") &&
            router_.has_ws_route(req.uri)) {
            // The parser stopped at the end of the handshake: anything still
            // buffered is WebSocket data.

            auto writer = std::dynamic_pointer_cast<http::ResponseWriter>(shared_from_this());
            auto reject = [&](const std::string& message) {
                http::HttpResponse err;
                err.status_code = http::HttpStatus::BadRequest;
                err.headers["Sec-WebSocket-Version"] = "13";
                err.set_body(message);
                should_close_ = true;
                send(std::move(err));
            };

            // RFC 6455 section 4.2.1 opening handshake requirements.
            auto conn_it = req.headers.find("Connection");
            auto key_it = req.headers.find("Sec-WebSocket-Key");
            auto version_it = req.headers.find("Sec-WebSocket-Version");
            if (req.method != http::HttpMethod::GET ||
                conn_it == req.headers.end() || !http::connection_option_present(conn_it->second, "upgrade") ||
                key_it == req.headers.end() || !http::websocket::Handshake::is_valid_client_key(key_it->second)) {
                reject("400 Bad Request: invalid WebSocket handshake");
                return;
            }
            if (version_it == req.headers.end() || version_it->second != "13") {
                reject("400 Bad Request: unsupported WebSocket version");
                return;
            }

            // Authentication, rate limiting and origin checks apply to
            // WebSocket routes too. A middleware that stops the request has
            // already sent its response, so the upgrade is simply abandoned.
            should_close_ = true;
            if (!router_.run_ws_middlewares(req.uri, req, writer)) {
                return;
            }
            should_close_ = false;

            std::string accept_key = http::websocket::Handshake::generate_accept_key(std::string(key_it->second));
            
            http::HttpResponse res;
            res.status_code = http::HttpStatus::SwitchingProtocols;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                for (const auto& [k, v] : default_headers_) res.headers[k] = v;
            }
            res.headers["Upgrade"] = "websocket";
            res.headers["Connection"] = "Upgrade";
            res.headers["Sec-WebSocket-Accept"] = accept_key;
            
            bool use_deflate = false;
            auto ext_it = req.headers.find("Sec-WebSocket-Extensions");
            if (ext_it != req.headers.end() && ext_it->second.find("permessage-deflate") != std::string::npos) {
                use_deflate = true;
                res.headers["Sec-WebSocket-Extensions"] = "permessage-deflate; client_no_context_takeover; server_no_context_takeover";
            }
            
            std::string handshake_str = res.serialize();
            
            auto ws_conn = std::make_unique<http::websocket::WebSocketConnection>(*this, use_deflate);
            ws_conn->set_max_message_size(limits_.websocket_max_message_size);
            auto handler = router_.get_ws_route(req.uri);
            
            // Queue the handshake immediately
            write_raw(std::vector<char>(handshake_str.begin(), handshake_str.end()));
            
            // Modify the state!
            upgrade_to_websocket(std::move(ws_conn));
            arm_timer(timeouts_.websocket_idle);
            
            // Call the user callback (this allows them to set up on_message handlers and send initial messages)
            handler(*ws_connection_);
            
            // If the client pipelined a WebSocket frame immediately, process it!
            {
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (!read_buffer_.empty()) {
                    ws_connection_->process_raw_data(read_buffer_);
                }
            }

            // The HTTP request path only re-arms the read once a response
            // has been sent, which never happens for an upgrade. Without
            // this, frames the client sends after the handshake are never read.
            trigger_read();
            
            return; // Bypass standard HTTP routing
        }

        // Decide whether the connection persists after this response.
        //
        // RFC 9112 section 9.3: HTTP/1.1 is persistent by default and closes
        // only when a "close" connection option is present. HTTP/1.0 is the
        // other way round — it closes by default and persists only when the
        // client explicitly sends "keep-alive". Treating an HTTP/1.0 request
        // with no Connection header as persistent leaves the client waiting
        // for an end-of-message it will never see; ApacheBench without -k is
        // exactly this case and hangs until it times out.
        //
        // The connection option is a comma-separated list of case-insensitive
        // tokens, so compare tokens rather than the whole field value.
        auto it = req.headers.find("Connection");
        const bool has_close = (it != req.headers.end()) &&
                               http::connection_option_present(it->second, "close");
        const bool has_keep_alive = (it != req.headers.end()) &&
                                    http::connection_option_present(it->second, "keep-alive");
        const bool is_http_10 = (req.http_version == "HTTP/1.0");

        if (has_close) {
            should_close_ = true;
        } else if (is_http_10 && !has_keep_alive) {
            should_close_ = true;
        }
        
        auto writer = std::dynamic_pointer_cast<http::ResponseWriter>(shared_from_this());
        router_.route(req, writer);
    } else {
        http::HttpResponse err_res;
        err_res.status_code = http::HttpStatus::BadRequest;
        err_res.set_body("400 Bad Request");
        err_res.headers["Connection"] = "close";
        should_close_ = true;
        
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            read_buffer_.clear();
        }
        send(std::move(err_res));
    }
}

void Connection::send_error(http::HttpStatus status, const std::string& message) {
    http::HttpResponse err_res;
    err_res.status_code = status;
    err_res.set_body(message);
    err_res.headers["Connection"] = "close";
    should_close_ = true;
    
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_buffer_.clear();
    }
    
    send(std::move(err_res));
}

void Connection::set_header(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    default_headers_[key] = value;
}

// set_header() may run on another thread (e.g. middleware on a worker while
// a previous response is still being sent), so copy under its lock.
std::unordered_map<std::string, std::string> Connection::default_headers_snapshot() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return default_headers_;
}

void Connection::add_interceptor(std::function<void(http::HttpResponse&)> interceptor) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    interceptors_.push_back(std::move(interceptor));
}

void Connection::send_headers(http::HttpResponse& response) {
    mark_responded();
    // Interceptors see streamed responses too, as on HTTP/2 (access logs,
    // metrics and session cookies would otherwise miss them).
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        for (auto& interceptor : interceptors_) {
            interceptor(response);
        }
    }
    for (const auto& [k, v] : default_headers_snapshot()) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }

    if (manager_.shutting_down()) should_close_ = true;
    if (should_close_) {
        response.headers["Connection"] = "close";
    } else {
        response.headers["Connection"] = "keep-alive";
    }
    
    // Default to chunked transfer if no Content-Length
    if (response.headers.find("Content-Length") == response.headers.end()) {
        response.headers["Transfer-Encoding"] = "chunked";
        response_chunked_ = true;
    } else {
        response_chunked_ = false;
    }
    
    std::string serialized = response.serialize_headers();
    send_data(serialized);
    // A streamed response can last indefinitely (SSE). Keep a read armed so
    // that a client leaving is noticed now, not at the next failed write,
    // which an idle stream may never make (#197). Bytes that arrive
    // meanwhile stay buffered for the next request, as they always have.
    if (state_ == ConnectionState::HTTP) trigger_read();
}

void Connection::send(http::HttpResponse&& response) {
    mark_responded();
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        for (auto& interceptor : interceptors_) {
            interceptor(response);
        }
    }

    for (const auto& [k, v] : default_headers_snapshot()) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }

    if (manager_.shutting_down()) should_close_ = true;
    if (should_close_) {
        response.headers["Connection"] = "close";
    } else {
        response.headers["Connection"] = "keep-alive";
    }
    
    bool is_file = (response.file_fd != -1);
    std::string serialized_data;
    
    if (is_head_request_) {
        // Same headers as GET (including Content-Length), no content; an
        // open file is closed by the response's destructor.
        serialized_data = response.serialize_headers();
    } else if (is_file) {
        serialized_data = response.serialize_headers();
    } else {
        serialized_data = response.serialize();
    }

    // Close once this response is on the wire. Deciding this here, not when
    // the request was parsed, keeps an earlier pipelined response's write
    // completion from closing the socket before this one is queued.
    const bool last_response = should_close_;

    // The connection holds one file at a time. The next pipelined request
    // is processed only once this file is fully written; otherwise its
    // response would overwrite the file state mid-transfer (or put its
    // headers on the wire before this file's bytes).
    const bool sends_file = is_file && !is_head_request_;
    if (sends_file) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        file_fd_ = response.file_fd;
        file_size_ = response.file_size;
        file_offset_ = response.file_offset;
        response.file_fd = -1; // Prevent the destructor from closing it
        if (!last_response) resume_after_write_ = true;
    }

    send_data(serialized_data, last_response);

    if (last_response || sends_file) {
        return; // Closing, or continued by trigger_write() once the file is sent.
    }
    continue_after_response();
}

// Moves on after a complete response: serves the next pipelined request if
// it has already arrived, otherwise goes back to reading.
void Connection::continue_after_response() {
    bool body_unread = false;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        // A streamed body the handler answered before reading to its end:
        // the rest of it is still on the wire, so this connection cannot
        // carry another request.
        body_unread = streaming_request_ && !request_complete_;
        if (parser_ && request_complete_) parser_->next();
        message_started_ = false;
        headers_parsed_ = false;
        request_complete_ = false;
        streaming_request_ = false;
    }
    // The previous request's body stream is over, whatever happened to it.
    if (state_ == ConnectionState::HTTP_STREAMING_BODY) state_ = ConnectionState::HTTP;
    if (body_unread) {
        send_data("", true); // close once the response has been written
        return;
    }

    RequestState state = check_request_state();
    if (state == RequestState::COMPLETE || state == RequestState::HEADERS_COMPLETE) {
        // We already hold is_processing_request_ == true from the current request
        if (state == RequestState::HEADERS_COMPLETE) {
            state_ = ConnectionState::HTTP_STREAMING_BODY;
            send_continue_if_expected();
        }
        auto self = shared_from_this();
        thread_pool_.enqueue([self]() {
            self->process_request();
        });
        if (state == RequestState::HEADERS_COMPLETE) trigger_read();
    } else if (state == RequestState::ERROR_PAYLOAD_TOO_LARGE) {
        send_error(http::HttpStatus::PayloadTooLarge, "413 Payload Too Large");
    } else if (state == RequestState::ERROR_HEADERS_TOO_LARGE) {
        send_error(http::HttpStatus::RequestHeaderFieldsTooLarge, "431 Request Header Fields Too Large");
    } else if (state == RequestState::ERROR_BAD_REQUEST) {
        send_error(http::HttpStatus::BadRequest, "400 Bad Request");
    } else if (state == RequestState::ERROR_NOT_IMPLEMENTED) {
        send_error(http::HttpStatus::NotImplemented, "501 Not Implemented");
    } else {
        is_processing_request_ = false;
        arm_timer_for_current_phase();
        // Bytes may have been buffered (not parsed) while the handler ran,
        // and more may arrive between releasing the flag and this check.
        if (check_request_state() == RequestState::COMPLETE) {
            bool expected = false;
            if (is_processing_request_.compare_exchange_strong(expected, true)) {
                auto self = shared_from_this();
                thread_pool_.enqueue([self]() {
                    self->process_request();
                });
            }
            return;
        }
        trigger_read();
    }
}

void Connection::write_chunk(std::string_view chunk) {
    mark_responded();
    if (is_head_request_) return;
    if (response_chunked_) {
        std::string formatted_chunk;
        char hex_len[32];
        snprintf(hex_len, sizeof(hex_len), "%zx\r\n", chunk.size());
        formatted_chunk += hex_len;
        formatted_chunk += chunk;
        formatted_chunk += "\r\n";
        send_data(formatted_chunk);
    } else {
        send_data(chunk);
    }
}

void Connection::end() {
    mark_responded();
    const bool last_response = should_close_;
    // Closes once everything queued is written.
    send_data(response_chunked_ && !is_head_request_ ? "0\r\n\r\n" : "", last_response);
    if (last_response) {
        return;
    }
    continue_after_response();
}

void Connection::send_sse_event(std::string_view data, std::string_view event, std::string_view id) {
    mark_responded();
    std::string sse_msg;
    if (!event.empty()) sse_msg += "event: " + std::string(event) + "\n";
    if (!id.empty()) sse_msg += "id: " + std::string(id) + "\n";
    
    size_t start = 0;
    while (start < data.size()) {
        size_t end_pos = data.find('\n', start);
        if (end_pos == std::string_view::npos) {
            sse_msg += "data: " + std::string(data.substr(start)) + "\n";
            break;
        } else {
            sse_msg += "data: " + std::string(data.substr(start, end_pos - start)) + "\n";
            start = end_pos + 1;
        }
    }
    sse_msg += "\n";
    write_chunk(sse_msg);
}

void Connection::send_continue_if_expected() {
    if (!expect_continue_ || continue_sent_) return;
    continue_sent_ = true;
    send_data("HTTP/1.1 100 Continue\r\n\r\n");
}

void Connection::send_data(std::string_view data, bool close_after) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
        // Same critical section as the data, so the writer never sees the
        // close request without the final bytes it belongs after.
        if (close_after) close_after_write_ = true;
    }
    trigger_write();
}

void Connection::trigger_write() {
    bool expected = false;
    if (!is_writing_.compare_exchange_strong(expected, true)) {
        return; // Already writing
    }

    if (ssl_) {
        std::vector<char> chunk_to_encrypt;
        {
            std::lock_guard<std::mutex> lock(write_mutex_);
            if (!write_buffer_.empty()) {
                chunk_to_encrypt = std::move(write_buffer_);
                write_buffer_.clear();
            }
        }

        bool file_read_failed = false;
        {
            std::lock_guard<std::mutex> tls_lock(tls_mutex_);
            if (!chunk_to_encrypt.empty()) {
                SSL_write(ssl_, chunk_to_encrypt.data(), static_cast<int>(chunk_to_encrypt.size()));
            }
            drain_tls_output_locked();
            
            if (file_fd_ != -1 && file_offset_ < file_size_ && tls_write_buffer_.empty() && tls_inflight_.empty()) {
                char file_buf[16384];
                size_t to_read = static_cast<size_t>(std::min(static_cast<off_t>(sizeof(file_buf)), file_size_ - file_offset_));
                ssize_t bytes_read = utils::file::pread(file_fd_, file_buf, to_read, file_offset_);
                if (bytes_read > 0) {
                    SSL_write(ssl_, file_buf, static_cast<int>(bytes_read));
                    file_offset_ += bytes_read;
                    drain_tls_output_locked();
                } else {
                    file_read_failed = true;
                }
            }

            if (tls_inflight_.empty() && !tls_write_buffer_.empty()) {
                tls_inflight_.swap(tls_write_buffer_);
            }
        }

        if (file_read_failed) {
            is_writing_ = false;
            manager_.remove_connection(socket_.fd());
            return;
        }
        
        if (file_fd_ != -1 && file_offset_ >= file_size_) {
            utils::file::close(file_fd_);
            file_fd_ = -1;
        }
        
        // tls_inflight_ belongs to this writer until on_write_complete, so it
        // is safe to hand its storage to the proactor outside the lock.
        if (!tls_inflight_.empty()) {
            auto self = shared_from_this();
            if (submit_io([&] {
                    proactor_.async_write(socket_.fd(), tls_inflight_.data(), tls_inflight_.size(), [self](ssize_t written) {
                        self->on_write_complete(written);
                    });
                })) {
                return; // Will clear flag in callback
            }
            is_writing_ = false;
            return;
        }
    } else {
        std::lock_guard<std::mutex> lock(write_mutex_);
        
        // Move data from write_buffer_ to active_write_buffer_ if active is empty
        if (active_write_buffer_.empty() && !write_buffer_.empty()) {
            active_write_buffer_ = std::move(write_buffer_);
            write_buffer_.clear();
        }
        
        if (!active_write_buffer_.empty()) {
            auto self = shared_from_this();
            if (!submit_io([&] {
                    proactor_.async_write(socket_.fd(), active_write_buffer_.data(), active_write_buffer_.size(), [self](ssize_t written) {
                        self->on_write_complete(written);
                    });
                })) {
                is_writing_ = false;
            }
            return;
        }
        
        if (file_fd_ != -1 && file_size_ > file_offset_) {
            auto self = shared_from_this();
            if (!submit_io([&] {
                    proactor_.async_sendfile(socket_.fd(), file_fd_, file_offset_, static_cast<size_t>(file_size_ - file_offset_), [self](ssize_t written) {
                        self->on_sendfile_complete(written);
                    });
                })) {
                is_writing_ = false;
            }
            return;
        }
        
        if (file_fd_ != -1 && file_offset_ >= file_size_) {
            utils::file::close(file_fd_);
            file_fd_ = -1;
        }
    }
    
    is_writing_ = false;

    // A response queued by another thread between our last look at the
    // buffers and clearing is_writing_ saw "already writing" and left it to
    // us. Pick it up, or it would sit unsent until the next write.
    bool close_requested = false;
    bool resume = false;
    if (has_pending_output(close_requested, resume)) {
        trigger_write();
        return;
    }
    
    if (close_requested) {
        manager_.remove_connection(socket_.fd());
        return;
    }

    if (resume) {
        // A file response has been fully sent: move on to the next request.
        continue_after_response();
        return;
    }

    // Everything queued has been written; go back to waiting on the peer.
    if (!is_message_stream()) arm_timer_for_current_phase();
}

bool Connection::has_pending_output(bool& close_requested, bool& resume) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    close_requested = close_after_write_;
    bool pending = !write_buffer_.empty() || !active_write_buffer_.empty() ||
                   (file_fd_ != -1 && file_offset_ < file_size_);
    if (!pending && ssl_) {
        std::lock_guard<std::mutex> tls_lock(tls_mutex_);
        pending = !tls_write_buffer_.empty();
    }
    // Claimed here, under the lock, so exactly one caller resumes.
    if (!pending && resume_after_write_) {
        resume_after_write_ = false;
        resume = true;
    }
    return pending;
}

void Connection::on_write_complete(ssize_t bytes_written) {
    if (bytes_written < 0 && would_block(bytes_written)) {
        // Socket buffer full: wait for writability and resend what is in flight.
        is_writing_ = false;
        trigger_write();
        return;
    }
    if (bytes_written <= 0) {
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    // A write made progress; the peer is still reading. Not for WebSocket
    // or raw streams: there, only traffic from the peer proves it is alive
    // (our own pings must not keep a dead peer's connection open).
    if (!is_message_stream()) arm_timer(timeouts_.idle);
    
    if (ssl_) {
        size_t n = std::min(static_cast<size_t>(bytes_written), tls_inflight_.size());
        tls_inflight_.erase(tls_inflight_.begin(), tls_inflight_.begin() + static_cast<std::ptrdiff_t>(n));
    } else {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (static_cast<size_t>(bytes_written) <= active_write_buffer_.size()) {
            active_write_buffer_.erase(active_write_buffer_.begin(), active_write_buffer_.begin() + bytes_written);
        } else {
            active_write_buffer_.clear();
        }
    }
    
    is_writing_ = false;
    trigger_write(); // Try writing again if needed
}

void Connection::on_sendfile_complete(ssize_t bytes_written) {
    if (bytes_written < 0 && would_block(bytes_written)) {
        is_writing_ = false;
        trigger_write();
        return;
    }
    if (bytes_written <= 0) {
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    arm_timer(timeouts_.idle);
    file_offset_ += bytes_written;
    
    is_writing_ = false;
    trigger_write();
}

void Connection::drain_tls_output_locked() {
    char buf[4096];
    while (true) {
        int bytes = BIO_read(wbio_, buf, sizeof(buf));
        if (bytes <= 0) break;
        tls_write_buffer_.insert(tls_write_buffer_.end(), buf, buf + bytes);
    }
}

void Connection::write_raw(const std::vector<char>& data) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }
    trigger_write();
}

void Connection::ping_if_websocket() {
    if (state_ == ConnectionState::WEBSOCKET && ws_connection_) ws_connection_->ping();
}

void Connection::on_server_shutdown() {
    const bool first = !shutdown_notified_;
    shutdown_notified_ = true;
    switch (state_.load()) {
        case ConnectionState::WEBSOCKET:
            if (first && ws_connection_) ws_connection_->close(1001); // going away
            return;
        case ConnectionState::HTTP2:
            if (h2_session_) {
                if (first) h2_session_->begin_shutdown();
                if (h2_session_->idle()) mark_for_close();
            }
            return;
        case ConnectionState::RAW_STREAM:
        case ConnectionState::HTTP_STREAMING_BODY:
            // Long-lived by nature; closed at the shutdown deadline.
            should_close_ = true;
            return;
        default:
            break;
    }
    // HTTP/1.x: the response in progress (if any) is the last one.
    should_close_ = true;
    bool idle;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        // A request whose bytes are still arriving is in flight even though
        // the parser has drained them from read_buffer_.
        idle = !is_processing_request_ && read_buffer_.empty() && !message_started_;
    }
    if (idle) mark_for_close();
}

void Connection::mark_for_close() {
    should_close_ = true;
    send_data({}, true);
}

bool Connection::is_message_stream() const {
    ConnectionState s = state_.load();
    return s == ConnectionState::WEBSOCKET || s == ConnectionState::RAW_STREAM;
}

void Connection::upgrade_to_websocket(std::unique_ptr<http::websocket::WebSocketConnection> ws_conn) {
    state_ = ConnectionState::WEBSOCKET;
    ws_connection_ = std::move(ws_conn);
    arm_timer(timeouts_.websocket_idle); // from now on only the peer's frames extend it
}

void Connection::upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) {
    state_ = ConnectionState::RAW_STREAM;
    arm_timer(timeouts_.websocket_idle);
    raw_stream_on_data_ = std::move(on_data);
    raw_stream_on_close_ = std::move(on_close);
    
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        if (!read_buffer_.empty() && raw_stream_on_data_) {
            raw_stream_on_data_(std::string_view(read_buffer_.data(), read_buffer_.size()));
            read_buffer_.clear();
        }
    }
    trigger_read();
}

void Connection::read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) {
    bool already_over = false;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        body_stream_on_data_ = std::move(on_data);
        body_stream_on_end_ = std::move(on_end);
        // A stream route whose request carried no body completed with its
        // headers: there is nothing to stream.
        already_over = !streaming_request_;
    }
    if (already_over) {
        if (auto end = std::move(body_stream_on_end_)) end();
        return;
    }
    process_streaming_data();
}

void Connection::process_streaming_data() {
    bool finished = false;
    RequestState error = RequestState::INCOMPLETE;
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        // Nothing is parsed until the handler has asked for the body; until
        // then it stays buffered.
        if (!streaming_request_ || request_complete_ || !body_stream_on_data_ || read_buffer_.empty()) return;
        parser_->set_body_handler(body_stream_on_data_); // chunks are delivered under the lock
        size_t used = 0;
        auto event = parser_->feed(std::string_view(read_buffer_.data(), read_buffer_.size()), used);
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(used));
        if (event == http::Http1Parser::Event::MessageComplete) {
            request_complete_ = true;
            finished = true;
        } else if (event == http::Http1Parser::Event::Error) {
            error = parser_->error_status() == 413 ? RequestState::ERROR_PAYLOAD_TOO_LARGE
                                                   : RequestState::ERROR_BAD_REQUEST;
        }
    }
    // Outside the lock: the handler typically answers from on_end, and the
    // end of a response re-enters the parser (continue_after_response).
    if (finished) {
        if (auto end = std::move(body_stream_on_end_)) end();
    } else if (error == RequestState::ERROR_PAYLOAD_TOO_LARGE) {
        send_error(http::HttpStatus::PayloadTooLarge, "413 Payload Too Large");
    } else if (error == RequestState::ERROR_BAD_REQUEST) {
        send_error(http::HttpStatus::BadRequest, "400 Bad Request");
    }
}

// Feeds buffered bytes to the parser and reports where the current request
// stands. Bytes the parser used are removed from read_buffer_; what is left
// belongs to a later request or, after an upgrade, to the new protocol.
// Callers must not call this while a handler is using the parsed request,
// except continue_after_response(), which has just finished with it.
RequestState Connection::check_request_state() {
    std::lock_guard<std::mutex> lock(read_mutex_);
    if (!parser_) {
        http::Http1Parser::Limits limits;
        limits.max_request_line = limits_.max_request_line;
        limits.max_header_bytes = limits_.max_header_bytes;
        limits.max_headers = limits_.max_headers;
        limits.max_body_size = max_body_size_;
        parser_ = std::make_unique<http::Http1Parser>(limits);
        // Stream routes exist for uploads of any size: the handler reads the
        // body, so max_body_size does not apply to them.
        const routing::Router* router = &router_;
        const size_t max_body = max_body_size_;
        parser_->set_body_limit([router, max_body](const http::HttpRequest& r) {
            return router->is_stream_route(r.method, r.uri) ? std::numeric_limits<size_t>::max() : max_body;
        });
    }
    if (request_complete_) return RequestState::COMPLETE;
    if (streaming_request_) return RequestState::HEADERS_COMPLETE;

    using Event = http::Http1Parser::Event;
    RequestState result = RequestState::INCOMPLETE;
    size_t pos = 0;
    // At most two passes: HeadersComplete once, then the body.
    for (int pass = 0; pass < 2; ++pass) {
        size_t used = 0;
        Event event = parser_->feed(std::string_view(read_buffer_.data() + pos, read_buffer_.size() - pos), used);
        pos += used;
        if (used > 0) message_started_ = true;
        if (event == Event::NeedMore) break;
        if (event == Event::MessageComplete) {
            message_started_ = true;
            request_complete_ = true;
            result = RequestState::COMPLETE;
            break;
        }
        if (event == Event::Error) {
            switch (parser_->error_status()) {
                case 413: result = RequestState::ERROR_PAYLOAD_TOO_LARGE; break;
                case 431: result = RequestState::ERROR_HEADERS_TOO_LARGE; break;
                case 501: result = RequestState::ERROR_NOT_IMPLEMENTED; break;
                default:  result = RequestState::ERROR_BAD_REQUEST; break;
            }
            break;
        }
        // HeadersComplete: a body follows.
        headers_parsed_ = true;
        const http::HttpRequest& req = parser_->request();
        // Only HTTP/1.1 clients may be sent an interim 100 (RFC 9110 section 10.1.1).
        expect_continue_ = parser_->expect_continue() && req.http_version == "HTTP/1.1";
        if (router_.is_stream_route(req.method, req.uri)) {
            streaming_request_ = true;
            result = RequestState::HEADERS_COMPLETE;
            break;
        }
        // Otherwise read the body into the request: parse on.
    }
    read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(pos));
    return result;
}

} // namespace server
