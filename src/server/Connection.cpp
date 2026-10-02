#include <orbit/server/Connection.hpp>
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
#ifdef _WIN32
#include <io.h>
#define close _close
#define open _open
inline ssize_t pread(int fd, void* buf, size_t count, long offset) {
    _lseek(fd, offset, SEEK_SET);
    return _read(fd, buf, static_cast<unsigned int>(count));
}
#endif


namespace server {

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
    if (current_timer_id_ != 0) {
        timer_manager_.cancel_timer(current_timer_id_);
    }
    if (ssl_) {
        SSL_free(ssl_);
    }
    if (file_fd_ != -1) {
        close(file_fd_);
    }
}

void Connection::reset_timer() {
    if (current_timer_id_ != 0) {
        timer_manager_.cancel_timer(current_timer_id_);
    }
    current_timer_id_ = timer_manager_.add_timer(socket_.fd(), std::chrono::seconds(10));
}

void Connection::start() {
    reset_timer();
    trigger_read();
}

void Connection::trigger_read() {
    bool expected = false;
    if (is_reading_.compare_exchange_strong(expected, true)) {
        auto self = shared_from_this();
        proactor_.async_read(socket_.fd(), async_read_buf_, sizeof(async_read_buf_), [self](ssize_t bytes) {
            self->is_reading_ = false;
            self->on_read_complete(bytes);
        });
    }
}

void Connection::on_read_complete(ssize_t bytes_read) {
    if (bytes_read <= 0) {
        if (state_ == ConnectionState::RAW_STREAM && raw_stream_on_close_) {
            raw_stream_on_close_();
        }
        if (state_ == ConnectionState::HTTP_STREAMING_BODY && body_stream_on_end_) {
            body_stream_on_end_();
        }
        std::cout << "on_read_complete closed with bytes_read=" << bytes_read << std::endl;
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    reset_timer();
    
    if (ssl_) {
        BIO_write(rbio_, async_read_buf_, static_cast<int>(bytes_read));
        
        if (!is_tls_handshake_complete_) {
            int ret = SSL_do_handshake(ssl_);
            if (ret == 1) {
                is_tls_handshake_complete_ = true;
                const unsigned char* alpn = nullptr;
                unsigned int alpn_len = 0;
                SSL_get0_alpn_selected(ssl_, &alpn, &alpn_len);
                if (alpn_len == 2 && std::memcmp(alpn, "h2", 2) == 0) {
                    state_ = ConnectionState::HTTP2;
                    h2_session_ = std::make_shared<http::h2::Http2Session>(*this, router_, thread_pool_);
                }
            } else {
                int err = SSL_get_error(ssl_, ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    char wbuf[4096];
                    while (true) {
                        int wbytes = BIO_read(wbio_, wbuf, sizeof(wbuf));
                        if (wbytes <= 0) break;
                        tls_write_buffer_.insert(tls_write_buffer_.end(), wbuf, wbuf + wbytes);
                    }
                    if (!tls_write_buffer_.empty()) {
                        trigger_write();
                    }
                    trigger_read(); // Continue reading handshake data
                    return;
                } else {
                    manager_.remove_connection(socket_.fd());
                    return;
                }
            }
        }
        
        if (is_tls_handshake_complete_) {
            while (true) {
                char clear_buf[8192];
                int ret = SSL_read(ssl_, clear_buf, sizeof(clear_buf));
                if (ret > 0) {
                    std::lock_guard<std::mutex> lock(read_mutex_);
                    read_buffer_.insert(read_buffer_.end(), clear_buf, clear_buf + ret);
                } else {
                    break;
                }
            }
            
            char wbuf[4096];
            while (true) {
                int wbytes = BIO_read(wbio_, wbuf, sizeof(wbuf));
                if (wbytes <= 0) break;
                tls_write_buffer_.insert(tls_write_buffer_.end(), wbuf, wbuf + wbytes);
            }
        }
    } else {
        std::lock_guard<std::mutex> lock(read_mutex_);
        read_buffer_.insert(read_buffer_.end(), async_read_buf_, async_read_buf_ + bytes_read);
    }
    
    // We defer checking max body size to check_request_state()
    
    if (!tls_write_buffer_.empty()) {
        trigger_write();
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
    
    RequestState state = check_request_state();
    
    if (state == RequestState::COMPLETE || state == RequestState::HEADERS_COMPLETE) {
        bool expected = false;
        if (is_processing_request_.compare_exchange_strong(expected, true)) {
            if (state == RequestState::HEADERS_COMPLETE) {
                state_ = ConnectionState::HTTP_STREAMING_BODY;
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
    } else {
        trigger_read();
    }
}

void Connection::process_request() {
    default_headers_.clear();
    {
        std::lock_guard<std::mutex> lock(read_mutex_);
        current_request_buffer_ = std::string(read_buffer_.begin(), read_buffer_.end());
    }
    std::string_view raw_request(current_request_buffer_.data(), current_request_buffer_.size());
    auto parsed_req = http::HttpParser::parse(raw_request);
    
    if (parsed_req) {
        http::HttpRequest& req = *parsed_req;
        req.client_ip = client_ip_;
        
        // WebSocket Upgrade Interception
        auto upgrade_it = req.headers.find("Upgrade");
        if (upgrade_it != req.headers.end() &&
            http::connection_option_present(upgrade_it->second, "websocket") &&
            router_.has_ws_route(req.uri)) {
            // Consume the handshake request; anything after it is WebSocket data.
            {
                size_t consumed_bytes = raw_request.find("\r\n\r\n") + 4;
                std::lock_guard<std::mutex> lock(read_mutex_);
                if (consumed_bytes <= read_buffer_.size()) {
                    read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_bytes));
                } else {
                    read_buffer_.clear();
                }
            }

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
            auto handler = router_.get_ws_route(req.uri);
            
            // Queue the handshake immediately
            write_raw(std::vector<char>(handshake_str.begin(), handshake_str.end()));
            
            // Modify the state!
            upgrade_to_websocket(std::move(ws_conn));
            
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
        
        // Erase request from read buffer. The parser has already rejected
        // malformed framing, so Content-Length here is a validated number.
        size_t headers_end = raw_request.find("\r\n\r\n");
        size_t consumed_bytes = headers_end + 4;
        
        if (state_ != ConnectionState::HTTP_STREAMING_BODY) {
            size_t request_line_end = raw_request.find("\r\n");
            http::MessageFraming framing = http::parse_framing(
                raw_request.substr(request_line_end + 2, headers_end + 2 - (request_line_end + 2)));
            if (framing.chunked) {
                size_t encoded_size = 0;
                http::decode_chunked(raw_request.substr(headers_end + 4), request_body_storage_,
                                     encoded_size, max_body_size_);
                req.body = request_body_storage_;
                consumed_bytes += encoded_size;
            } else if (framing.has_content_length) {
                consumed_bytes += framing.content_length;
            }
        } else {
            req.body = {};
        }
        
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            if (consumed_bytes <= read_buffer_.size()) {
                read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_bytes));
            } else {
                read_buffer_.clear(); 
            }
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

void Connection::add_interceptor(std::function<void(http::HttpResponse&)> interceptor) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    interceptors_.push_back(std::move(interceptor));
}

void Connection::send_headers(http::HttpResponse& response) {
    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }

    if (should_close_) {
        response.headers["Connection"] = "close";
    } else {
        response.headers["Connection"] = "keep-alive";
    }
    
    // Default to chunked transfer if no Content-Length
    if (response.headers.find("Content-Length") == response.headers.end()) {
        response.headers["Transfer-Encoding"] = "chunked";
        is_chunked_ = true;
    } else {
        is_chunked_ = false;
    }
    
    std::string serialized = response.serialize_headers();
    send_data(serialized);
}

void Connection::send(http::HttpResponse&& response) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        for (auto& interceptor : interceptors_) {
            interceptor(response);
        }
    }

    for (const auto& [k, v] : default_headers_) {
        if (response.headers.find(k) == response.headers.end()) {
            response.headers[k] = v;
        }
    }

    if (should_close_) {
        response.headers["Connection"] = "close";
    } else {
        response.headers["Connection"] = "keep-alive";
    }
    
    bool is_file = (response.file_fd != -1);
    std::string serialized_data;
    
    if (is_file) {
        serialized_data = response.serialize_headers();
        this->file_fd_ = response.file_fd;
        this->file_size_ = response.file_size;
        this->file_offset_ = 0;
        response.file_fd = -1; // Prevent the destructor from closing it
    } else {
        serialized_data = response.serialize();
    }

    send_data(serialized_data);
    
    RequestState state = check_request_state();
    if (state == RequestState::COMPLETE) {
        // We already hold is_processing_request_ == true from the current request
        auto self = shared_from_this();
        thread_pool_.enqueue([self]() {
            self->process_request();
        });
    } else if (state == RequestState::ERROR_PAYLOAD_TOO_LARGE) {
        send_error(http::HttpStatus::PayloadTooLarge, "413 Payload Too Large");
    } else if (state == RequestState::ERROR_HEADERS_TOO_LARGE) {
        send_error(http::HttpStatus::RequestHeaderFieldsTooLarge, "431 Request Header Fields Too Large");
    } else if (state == RequestState::ERROR_BAD_REQUEST) {
        send_error(http::HttpStatus::BadRequest, "400 Bad Request");
    } else {
        is_processing_request_ = false;
        // Double check state after releasing the lock, just in case data was appended concurrently
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
    if (is_chunked_) {
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
    if (is_chunked_) {
        send_data("0\r\n\r\n");
    }
    
    RequestState state = check_request_state();
    if (state == RequestState::COMPLETE) {
        auto self = shared_from_this();
        thread_pool_.enqueue([self]() {
            self->process_request();
        });
    } else if (state == RequestState::ERROR_PAYLOAD_TOO_LARGE) {
        send_error(http::HttpStatus::PayloadTooLarge, "413 Payload Too Large");
    } else if (state == RequestState::ERROR_HEADERS_TOO_LARGE) {
        send_error(http::HttpStatus::RequestHeaderFieldsTooLarge, "431 Request Header Fields Too Large");
    } else if (state == RequestState::ERROR_BAD_REQUEST) {
        send_error(http::HttpStatus::BadRequest, "400 Bad Request");
    } else {
        is_processing_request_ = false;
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

void Connection::send_sse_event(std::string_view data, std::string_view event, std::string_view id) {
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

void Connection::send_data(std::string_view data) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
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
        if (!chunk_to_encrypt.empty()) {
            SSL_write(ssl_, chunk_to_encrypt.data(), static_cast<int>(chunk_to_encrypt.size()));
        }
        
        char buf[4096];
        while (true) {
            int bytes = BIO_read(wbio_, buf, sizeof(buf));
            if (bytes <= 0) break;
            tls_write_buffer_.insert(tls_write_buffer_.end(), buf, buf + bytes);
        }
        
        if (file_fd_ != -1 && file_offset_ < file_size_ && tls_write_buffer_.empty()) {
            char file_buf[16384];
            size_t to_read = static_cast<size_t>(std::min(static_cast<off_t>(sizeof(file_buf)), file_size_ - file_offset_));
            ssize_t bytes_read = pread(file_fd_, file_buf, to_read, file_offset_);
            
            if (bytes_read > 0) {
                SSL_write(ssl_, file_buf, static_cast<int>(bytes_read));
                file_offset_ += bytes_read;
                
                while (true) {
                    int wbytes = BIO_read(wbio_, buf, sizeof(buf));
                    if (wbytes <= 0) break;
                    tls_write_buffer_.insert(tls_write_buffer_.end(), buf, buf + wbytes);
                }
            } else {
                is_writing_ = false;
                manager_.remove_connection(socket_.fd());
                return;
            }
        }
        
        if (file_fd_ != -1 && file_offset_ >= file_size_) {
            close(file_fd_);
            file_fd_ = -1;
        }
        
        if (!tls_write_buffer_.empty()) {
            auto self = shared_from_this();
            proactor_.async_write(socket_.fd(), tls_write_buffer_.data(), tls_write_buffer_.size(), [self](ssize_t written) {
                self->on_write_complete(written);
            });
            return; // Will clear flag in callback
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
            proactor_.async_write(socket_.fd(), active_write_buffer_.data(), active_write_buffer_.size(), [self](ssize_t written) {
                self->on_write_complete(written);
            });
            return;
        }
        
        if (file_fd_ != -1 && file_size_ > file_offset_) {
            auto self = shared_from_this();
            proactor_.async_sendfile(socket_.fd(), file_fd_, file_offset_, static_cast<size_t>(file_size_ - file_offset_), [self](ssize_t written) {
                self->on_sendfile_complete(written);
            });
            return;
        }
        
        if (file_fd_ != -1 && file_offset_ >= file_size_) {
            close(file_fd_);
            file_fd_ = -1;
        }
    }
    
    is_writing_ = false;
    
    if (should_close_) {
        manager_.remove_connection(socket_.fd());
    }
}

void Connection::on_write_complete(ssize_t bytes_written) {
    if (bytes_written <= 0) {
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    reset_timer();
    
    if (ssl_) {
        tls_write_buffer_.erase(tls_write_buffer_.begin(), tls_write_buffer_.begin() + bytes_written);
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
    if (bytes_written <= 0) {
        manager_.remove_connection(socket_.fd());
        return;
    }
    
    reset_timer();
    file_offset_ += bytes_written;
    
    is_writing_ = false;
    trigger_write();
}

void Connection::write_raw(const std::vector<char>& data) {
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }
    trigger_write();
}

void Connection::mark_for_close() {
    should_close_ = true;
    trigger_write();
}

void Connection::upgrade_to_websocket(std::unique_ptr<http::websocket::WebSocketConnection> ws_conn) {
    state_ = ConnectionState::WEBSOCKET;
    ws_connection_ = std::move(ws_conn);
}

void Connection::upgrade_to_raw_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_close) {
    state_ = ConnectionState::RAW_STREAM;
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
    body_stream_on_data_ = std::move(on_data);
    body_stream_on_end_ = std::move(on_end);
    process_streaming_data();
}

void Connection::process_streaming_data() {
    std::lock_guard<std::mutex> lock(read_mutex_);
    if (read_buffer_.empty()) return;
    
    // Do not consume data until the handler has registered the callback!
    if (!body_stream_on_data_) return;
    
    if (is_chunked_) {
        while (!read_buffer_.empty()) {
            if (is_chunk_header_mode_) {
                std::string_view buf(read_buffer_.data(), read_buffer_.size());
                size_t crlf = buf.find("\r\n");
                if (crlf == std::string_view::npos) break; 
                
                std::string_view hex_str = buf.substr(0, crlf);
                try {
                    chunk_bytes_remaining_ = std::stoull(std::string(hex_str), nullptr, 16);
                } catch (...) { break; }
                
                read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + crlf + 2);
                is_chunk_header_mode_ = false;
                
                if (chunk_bytes_remaining_ == 0) {
                    if (body_stream_on_end_) {
                        auto on_end = std::move(body_stream_on_end_);
                        on_end();
                    }
                    if (read_buffer_.size() >= 2) read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + 2);
                    break;
                }
            } else {
                size_t available = read_buffer_.size();
                size_t to_read = std::min(available, chunk_bytes_remaining_);
                if (to_read > 0 && body_stream_on_data_) {
                    body_stream_on_data_(std::string_view(read_buffer_.data(), to_read));
                }
                read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + to_read);
                chunk_bytes_remaining_ -= to_read;
                
                if (chunk_bytes_remaining_ == 0) {
                    if (read_buffer_.size() >= 2) {
                        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + 2);
                        is_chunk_header_mode_ = true;
                    } else if (read_buffer_.size() == 1 && read_buffer_[0] == '\r') {
                        break;
                    } else if (read_buffer_.empty()) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
    } else {
        size_t available = read_buffer_.size();
        size_t to_read = std::min(available, content_length_remaining_);
        if (to_read > 0 && body_stream_on_data_) {
            body_stream_on_data_(std::string_view(read_buffer_.data(), to_read));
        }
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + to_read);
        
        if (content_length_remaining_ != static_cast<size_t>(-1)) {
            content_length_remaining_ -= to_read;
            if (content_length_remaining_ == 0 && body_stream_on_end_) {
                auto on_end = std::move(body_stream_on_end_);
                on_end();
            }
        }
    }
}

RequestState Connection::check_request_state() {
    std::lock_guard<std::mutex> lock(read_mutex_);
    std::string_view buf_view(read_buffer_.data(), read_buffer_.size());
    size_t headers_end = buf_view.find("\r\n\r\n");
    
    if (headers_end == std::string_view::npos) {
        // If we haven't found headers end, check if headers are too large
        if (read_buffer_.size() > 8192) {
            return RequestState::ERROR_HEADERS_TOO_LARGE;
        }
        return RequestState::INCOMPLETE;
    }
    
    // Check if URI is too long (first line)
    size_t first_line_end = buf_view.find("\r\n");
    if (first_line_end != std::string_view::npos && first_line_end > 4096) {
        return RequestState::ERROR_HEADERS_TOO_LARGE;
    }
    if (headers_end > 8192) {
        return RequestState::ERROR_HEADERS_TOO_LARGE;
    }

    // Framing is decided once, strictly, from the parsed header lines. Both
    // this check and process_request() use the same rules, so they can never
    // disagree about where a request ends.
    http::MessageFraming framing = http::parse_framing(
        buf_view.substr(first_line_end + 2, headers_end + 2 - (first_line_end + 2)));
    if (!framing.valid) {
        return RequestState::ERROR_BAD_REQUEST;
    }
    
    // -------------------------------------------------------------
    // Check if this route is a STREAM route. If so, return HEADERS_COMPLETE
    // -------------------------------------------------------------
    std::string_view request_line = buf_view.substr(0, first_line_end);
    size_t space1 = request_line.find(' ');
    size_t space2 = request_line.find(' ', space1 + 1);
    if (space1 != std::string_view::npos && space2 != std::string_view::npos && space1 != space2) {
        http::HttpMethod method = http::HttpParser::parse_method(request_line.substr(0, space1));
        std::string_view full_uri = request_line.substr(space1 + 1, space2 - space1 - 1);
        size_t q_mark = full_uri.find('?');
        std::string uri(q_mark != std::string_view::npos ? full_uri.substr(0, q_mark) : full_uri);
        
        if (router_.is_stream_route(method, uri)) {
            if (framing.chunked) {
                is_chunked_ = true;
                is_chunk_header_mode_ = true;
            } else {
                is_chunked_ = false;
                // Without Content-Length the stream runs until the peer closes,
                // matching the previous streaming-route behaviour.
                content_length_remaining_ = framing.has_content_length
                    ? framing.content_length
                    : static_cast<size_t>(-1);
            }
            return RequestState::HEADERS_COMPLETE;
        }
    }
    // -------------------------------------------------------------

    std::string_view body = buf_view.substr(headers_end + 4);

    if (framing.chunked) {
        std::string decoded;
        size_t consumed = 0;
        switch (http::decode_chunked(body, decoded, consumed, max_body_size_)) {
            case http::ChunkedStatus::Complete: return RequestState::COMPLETE;
            case http::ChunkedStatus::Incomplete: return RequestState::INCOMPLETE;
            case http::ChunkedStatus::TooLarge: return RequestState::ERROR_PAYLOAD_TOO_LARGE;
            case http::ChunkedStatus::Invalid: return RequestState::ERROR_BAD_REQUEST;
        }
    }

    if (framing.has_content_length) {
        if (framing.content_length > max_body_size_) {
            return RequestState::ERROR_PAYLOAD_TOO_LARGE;
        }
        return body.size() >= framing.content_length ? RequestState::COMPLETE : RequestState::INCOMPLETE;
    }

    // No Content-Length and no Transfer-Encoding: the body is empty (RFC 9112 section 6.3).
    return RequestState::COMPLETE;
}

} // namespace server
