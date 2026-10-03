#include <orbit/middleware/Proxy.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/network/PlatformSocket.hpp>


#ifndef _WIN32
#include <unistd.h>
#include <netdb.h>
#endif
#include <fcntl.h>


#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <orbit/network/ConnectionPool.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/network/TlsContext.hpp>

#ifdef _WIN32
#undef DELETE
#undef ERROR
#endif

namespace middleware {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Splits a comma-separated header value into lowercase tokens.
std::vector<std::string> lower_tokens(std::string_view value) {
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string_view token = trim(value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        if (!token.empty()) tokens.push_back(to_lower(token));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return tokens;
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// RFC 9110 section 7.6.1 hop-by-hop fields, plus fields the proxy sets itself.
bool is_hop_by_hop(const std::string& lower_name) {
    static const char* const names[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding", "te",
        "trailer", "upgrade", "proxy-authorization", "proxy-authenticate",
        "host", "content-length"
    };
    for (const char* n : names) {
        if (lower_name == n) return true;
    }
    return false;
}

bool is_forwarding_header(const std::string& lower_name) {
    return lower_name == "x-forwarded-for" || lower_name == "x-forwarded-host" ||
           lower_name == "x-forwarded-proto" || lower_name == "x-real-ip" || lower_name == "forwarded";
}

const char* method_name(http::HttpMethod method) {
    switch (method) {
        case http::HttpMethod::GET: return "GET";
        case http::HttpMethod::POST: return "POST";
        case http::HttpMethod::PUT: return "PUT";
        case http::HttpMethod::PATCH: return "PATCH";
        case http::HttpMethod::DELETE: return "DELETE";
        case http::HttpMethod::OPTIONS: return "OPTIONS";
        case http::HttpMethod::HEAD: return "HEAD";
        default: return nullptr;
    }
}

} // namespace

namespace detail {

ProxiedRequest snapshot_request(const http::HttpRequest& request) {
    ProxiedRequest copy;
    const char* method = method_name(request.method);
    copy.method = method ? method : "";
    copy.target = request.target.empty() ? request.uri : request.target;
    for (const auto& [k, v] : request.headers) {
        copy.headers.emplace_back(std::string(k), std::string(v));
    }
    copy.body = std::string(request.body);
    copy.client_ip = request.client_ip;
    return copy;
}

std::string build_upstream_request(const ProxiedRequest& request, const std::string& host, int port,
                                   const std::string& strip_prefix, bool trust_forwarded_headers) {
    std::string target = request.target.empty() ? "/" : request.target;
    if (!strip_prefix.empty() && target.compare(0, strip_prefix.size(), strip_prefix) == 0) {
        target = target.substr(strip_prefix.size());
        if (target.empty() || target[0] != '/') target = "/" + target;
    }

    // Headers named in Connection are hop-by-hop too.
    std::vector<std::string> connection_tokens;
    std::string client_host;
    std::string upgrade;
    std::string prior_xff, prior_real_ip, prior_forwarded_host;
    for (const auto& [k, v] : request.headers) {
        std::string lk = to_lower(k);
        if (lk == "connection") {
            auto t = lower_tokens(v);
            connection_tokens.insert(connection_tokens.end(), t.begin(), t.end());
        } else if (lk == "host") {
            client_host = v;
        } else if (lk == "upgrade") {
            upgrade = v;
        } else if (lk == "x-forwarded-for") {
            prior_xff = v;
        } else if (lk == "x-real-ip") {
            prior_real_ip = v;
        } else if (lk == "x-forwarded-host") {
            prior_forwarded_host = v;
        }
    }
    bool is_upgrade = !upgrade.empty() && contains(connection_tokens, "upgrade");

    std::string out;
    out += request.method + " " + target + " HTTP/1.1\r\n";
    out += "Host: " + host + ":" + std::to_string(port) + "\r\n";

    for (const auto& [k, v] : request.headers) {
        std::string lk = to_lower(k);
        if (is_hop_by_hop(lk) || contains(connection_tokens, lk) || is_forwarding_header(lk)) continue;
        out += k + ": " + v + "\r\n";
    }

    // Forwarding headers. Unless the operator says the client side is a
    // trusted proxy, client-supplied values are discarded: otherwise any
    // client could choose the address the upstream sees.
    std::string xff = request.client_ip;
    if (trust_forwarded_headers && !prior_xff.empty()) {
        xff = prior_xff + (request.client_ip.empty() ? "" : ", " + request.client_ip);
    }
    if (!xff.empty()) out += "X-Forwarded-For: " + xff + "\r\n";
    std::string real_ip = (trust_forwarded_headers && !prior_real_ip.empty()) ? prior_real_ip : request.client_ip;
    if (!real_ip.empty()) out += "X-Real-IP: " + real_ip + "\r\n";
    std::string fwd_host = (trust_forwarded_headers && !prior_forwarded_host.empty()) ? prior_forwarded_host : client_host;
    if (!fwd_host.empty()) out += "X-Forwarded-Host: " + fwd_host + "\r\n";

    if (!request.body.empty() || request.method == "POST" || request.method == "PUT" || request.method == "PATCH") {
        out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    }

    if (is_upgrade) {
        out += "Upgrade: " + upgrade + "\r\nConnection: Upgrade\r\n\r\n";
    } else {
        out += "Connection: keep-alive\r\n\r\n";
    }
    out += request.body;
    return out;
}

ChunkedDecoder::Result ChunkedDecoder::feed(std::string_view data, std::string& out) {
    size_t i = 0;
    while (i < data.size()) {
        switch (state_) {
            case State::Size: {
                char c = data[i++];
                if (c != '\n') {
                    line_ += c;
                    if (line_.size() > 1024) return Result::Error;
                    break;
                }
                std::string_view line = trim(line_);
                size_t ext = line.find(';');
                std::string_view hex = trim(ext == std::string_view::npos ? line : line.substr(0, ext));
                if (hex.empty() || hex.size() > 15) return Result::Error;
                size_t size = 0;
                for (char h : hex) {
                    int d;
                    if (h >= '0' && h <= '9') d = h - '0';
                    else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                    else return Result::Error;
                    size = (size << 4) | static_cast<size_t>(d);
                }
                line_.clear();
                remaining_ = size;
                state_ = size == 0 ? State::Trailer : State::Data;
                break;
            }
            case State::Data: {
                size_t n = std::min(remaining_, data.size() - i);
                out.append(data.substr(i, n));
                i += n;
                remaining_ -= n;
                if (remaining_ == 0) state_ = State::DataEnd;
                break;
            }
            case State::DataEnd: {
                char c = data[i++];
                if (c == '\n') state_ = State::Size;
                else if (c != '\r') return Result::Error;
                break;
            }
            case State::Trailer: {
                char c = data[i++];
                if (c != '\n') {
                    line_ += c;
                    if (line_.size() > 8192) return Result::Error;
                    break;
                }
                bool empty_line = trim(line_).empty();
                line_.clear();
                if (empty_line) {
                    state_ = State::Finished;
                    return Result::Done;
                }
                break;
            }
            case State::Finished:
                return Result::Done;
        }
    }
    return state_ == State::Finished ? Result::Done : Result::NeedMore;
}

} // namespace detail

namespace {

struct UpstreamConfig {
    std::string host;
    int port;
    bool use_tls;
    std::string strip_prefix;
    bool trust_forwarded_headers;
    std::shared_ptr<network::ClientTlsContext> tls;
};

class ProxyRequest : public std::enable_shared_from_this<ProxyRequest> {
public:
    ProxyRequest(network::Proactor& proactor, std::shared_ptr<http::ResponseWriter> writer,
                 UpstreamConfig upstream, detail::ProxiedRequest request)
        : proactor_(proactor), writer_(std::move(writer)), upstream_(std::move(upstream)),
          request_(std::move(request)) {
        is_head_ = (request_.method == "HEAD");
    }

    ~ProxyRequest() {
        if (ssl_) SSL_free(ssl_);
        if (fd_ != -1) {
            proactor_.remove(fd_);
            network::close_socket(fd_);
        }
    }

    void start() {
        if (request_.method.empty()) {
            fail("Unsupported request method");
            return;
        }

        auto [fd, ssl_ptr] = network::ConnectionPool::get_instance().acquire(upstream_.host, upstream_.port);
        if (fd != -1) {
            fd_ = fd;
            if (upstream_.use_tls) {
                ssl_ = static_cast<SSL*>(ssl_ptr);
                is_handshake_complete_ = true;
                if (ssl_) {
                    rbio_ = SSL_get_rbio(ssl_);
                    wbio_ = SSL_get_wbio(ssl_);
                }
            }
            is_reused_ = true;
            on_connected();
            return;
        }

        // Resolve on the thread pool; getaddrinfo is thread-safe, unlike gethostbyname.
        auto self = shared_from_this();
        writer_->thread_pool().enqueue([self]() {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            std::string port_str = std::to_string(self->upstream_.port);
            if (getaddrinfo(self->upstream_.host.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
                self->fail("DNS resolution failed");
                return;
            }
            sockaddr_in addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
            freeaddrinfo(result);

            self->fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
            if (self->fd_ < 0) {
                self->fail("Failed to create socket");
                return;
            }
#ifdef _WIN32
            u_long mode = 1;
            ioctlsocket(self->fd_, FIONBIO, &mode);
#else
            fcntl(self->fd_, F_SETFL, fcntl(self->fd_, F_GETFL, 0) | O_NONBLOCK);
            fcntl(self->fd_, F_SETFD, fcntl(self->fd_, F_GETFD, 0) | FD_CLOEXEC);
#endif

            if (self->upstream_.use_tls) {
                self->ssl_ = SSL_new(self->upstream_.tls->get());
                SSL_set_tlsext_host_name(self->ssl_, self->upstream_.host.c_str());
                // Check the certificate is for this host, not just any trusted one.
                SSL_set1_host(self->ssl_, self->upstream_.host.c_str());
                self->rbio_ = BIO_new(BIO_s_mem());
                self->wbio_ = BIO_new(BIO_s_mem());
                SSL_set_bio(self->ssl_, self->rbio_, self->wbio_);
                SSL_set_connect_state(self->ssl_);
            }

            self->proactor_.async_connect(self->fd_, addr, [self](int status) {
                if (status != 0) {
                    self->fail("Connection refused");
                    return;
                }
                self->on_connected();
            });
        });
    }

private:
    void fail(const std::string& message) {
        LOG_ERROR("Proxy error: " << message);
        if (headers_sent_) {
            writer_->end();
            return;
        }
        headers_sent_ = true;
        http::HttpResponse res;
        res.status(http::HttpStatus::InternalServerError).send("502 Bad Gateway: " + message);
        writer_->send(std::move(res));
    }

    void on_connected() {
        if (upstream_.use_tls && !is_handshake_complete_) {
            do_handshake();
            return;
        }
        send_request();
    }

    void do_handshake() {
        int ret = SSL_do_handshake(ssl_);
        if (ret == 1) {
            is_handshake_complete_ = true;
            send_request();
            return;
        }

        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            auto self = shared_from_this();
            flush_tls_writes([self]() {
                if (!self->is_handshake_complete_) {
                    self->read_loop();
                }
            });
        } else {
            long verify = SSL_get_verify_result(ssl_);
            fail(verify != X509_V_OK
                 ? std::string("TLS verification failed: ") + X509_verify_cert_error_string(verify)
                 : std::string("TLS handshake failed"));
        }
    }

    void flush_tls_writes(std::function<void()> on_flushed) {
        int pending = BIO_pending(wbio_);
        if (pending > 0) {
            auto buf = std::make_shared<std::vector<char>>(static_cast<size_t>(pending));
            int read_bytes = BIO_read(wbio_, buf->data(), pending);
            if (read_bytes > 0) {
                auto self = shared_from_this();
                proactor_.async_write(fd_, buf->data(), static_cast<size_t>(read_bytes), [self, buf, cb = std::move(on_flushed)](ssize_t bytes) {
                    if (bytes <= 0) {
                        self->fail("Failed to write to upstream");
                        return;
                    }
                    if (cb) cb();
                });
                return;
            }
        }
        if (on_flushed) on_flushed();
    }

    void send_request() {
        write_buf_ = detail::build_upstream_request(request_, upstream_.host, upstream_.port,
                                                    upstream_.strip_prefix, upstream_.trust_forwarded_headers);

        auto self = shared_from_this();
        if (upstream_.use_tls) {
            SSL_write(ssl_, write_buf_.data(), static_cast<int>(write_buf_.size()));
            write_buf_.clear();
            flush_tls_writes([self]() { self->read_loop(); });
            return;
        }

        proactor_.async_write(fd_, write_buf_.data(), write_buf_.size(), [self](ssize_t bytes) {
            if (bytes <= 0) {
                if (self->is_reused_) {
                    // A pooled connection the upstream already closed: retry on a fresh one.
                    self->proactor_.remove(self->fd_);
                    network::close_socket(self->fd_);
                    self->fd_ = -1;
                    self->is_reused_ = false;
                    self->start();
                    return;
                }
                self->fail("Failed to send request to upstream");
                return;
            }
            self->read_loop();
        });
    }

    void read_loop() {
        auto self = shared_from_this();
        proactor_.async_read(fd_, read_buf_, sizeof(read_buf_), [self](ssize_t bytes) {
            if (self->finished_) return;
            if (bytes > 0) {
                if (self->upstream_.use_tls) {
                    BIO_write(self->rbio_, self->read_buf_, static_cast<int>(bytes));
                    if (!self->is_handshake_complete_) {
                        self->do_handshake();
                        return;
                    }
                    self->process_tls_read();
                } else {
                    self->process_cleartext(self->read_buf_, static_cast<size_t>(bytes));
                }
            } else {
                if (self->is_websocket_) {
                    return; // Upstream closed the websocket
                }
                if (!self->headers_sent_) {
                    self->fail("Upstream closed the connection before sending a response");
                } else {
                    // Close-delimited body: EOF marks its end.
                    self->finished_ = true;
                    self->writer_->end();
                }
            }
        });
    }

    void process_tls_read() {
        char buf[16384];
        while (true) {
            int bytes = SSL_read(ssl_, buf, sizeof(buf));
            if (bytes > 0) {
                process_cleartext(buf, static_cast<size_t>(bytes));
                if (finished_) return;
            } else {
                break;
            }
        }
        flush_tls_writes(nullptr);
        if (!finished_) read_loop();
    }

    // Returns after scheduling at most one further read (plain connections).
    void process_cleartext(const char* data, size_t len) {
        if (is_websocket_) {
            writer_->write_chunk(std::string_view(data, len));
            if (!upstream_.use_tls) read_loop();
            return;
        }

        std::string_view body;
        if (!headers_sent_) {
            response_accumulator_.append(data, len);
            size_t header_end = response_accumulator_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if (response_accumulator_.size() > 64 * 1024) {
                    fail("Upstream response headers too large");
                    return;
                }
                if (!upstream_.use_tls) read_loop();
                return;
            }
            if (!parse_and_send_headers(header_end)) return;
            if (is_websocket_) {
                // Any bytes after the 101 headers are already WebSocket data.
                if (header_end + 4 < response_accumulator_.size()) {
                    writer_->write_chunk(std::string_view(response_accumulator_).substr(header_end + 4));
                }
                response_accumulator_.clear();
                if (!upstream_.use_tls) read_loop();
                return;
            }
            leftover_ = response_accumulator_.substr(header_end + 4);
            response_accumulator_.clear();
            body = leftover_;
        } else {
            body = std::string_view(data, len);
        }

        consume_body(body);
        if (!finished_ && !upstream_.use_tls) read_loop();
    }

    void consume_body(std::string_view body) {
        if (finished_) return;
        if (body_mode_ == BodyMode::None) {
            complete();
            return;
        }
        if (body_mode_ == BodyMode::Chunked) {
            std::string decoded;
            auto result = chunk_decoder_.feed(body, decoded);
            if (!decoded.empty()) writer_->write_chunk(decoded);
            if (result == detail::ChunkedDecoder::Result::Error) {
                keep_alive_ = false;
                fail("Malformed chunked response from upstream");
                finished_ = true;
            } else if (result == detail::ChunkedDecoder::Result::Done) {
                complete();
            }
            return;
        }
        if (body_mode_ == BodyMode::ContentLength) {
            size_t take = std::min(body.size(), content_length_ - body_bytes_read_);
            if (take > 0) writer_->write_chunk(body.substr(0, take));
            body_bytes_read_ += take;
            if (body_bytes_read_ >= content_length_) complete();
            return;
        }
        // Close-delimited
        if (!body.empty()) writer_->write_chunk(body);
    }

    void complete() {
        finished_ = true;
        writer_->end();
        if (keep_alive_) {
            network::ConnectionPool::get_instance().release(upstream_.host, upstream_.port, fd_, ssl_);
            proactor_.remove(fd_);
            fd_ = -1;       // Now owned by the pool
            ssl_ = nullptr;
        }
    }

    bool parse_and_send_headers(size_t header_end) {
        std::string_view head(response_accumulator_.data(), header_end);
        size_t line_end = head.find("\r\n");
        std::string_view status_line = head.substr(0, line_end);

        // "HTTP/1.x SSS reason"
        int status = 0;
        if (status_line.size() >= 12 && status_line.substr(0, 5) == "HTTP/" && status_line[8] == ' ' &&
            std::isdigit(static_cast<unsigned char>(status_line[9])) &&
            std::isdigit(static_cast<unsigned char>(status_line[10])) &&
            std::isdigit(static_cast<unsigned char>(status_line[11]))) {
            status = (status_line[9] - '0') * 100 + (status_line[10] - '0') * 10 + (status_line[11] - '0');
        }
        if (status < 100) {
            fail("Malformed status line from upstream");
            return false;
        }
        keep_alive_ = status_line.substr(5, 3) == "1.1";

        http::HttpResponse res;
        res.status_code = static_cast<http::HttpStatus>(status);

        bool chunked = false;
        bool has_length = false;
        std::vector<std::string> connection_tokens;
        std::vector<std::pair<std::string, std::string>> fields;

        size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
        while (pos < head.size()) {
            size_t next = head.find("\r\n", pos);
            std::string_view line = head.substr(pos, next == std::string_view::npos ? std::string_view::npos : next - pos);
            pos = next == std::string_view::npos ? head.size() : next + 2;

            size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) continue;
            std::string key(line.substr(0, colon));
            std::string value(trim(line.substr(colon + 1)));
            std::string lk = to_lower(key);

            if (lk == "content-length") {
                size_t n = 0;
                bool ok = !value.empty();
                for (char c : value) {
                    if (c < '0' || c > '9') { ok = false; break; }
                    n = n * 10 + static_cast<size_t>(c - '0');
                }
                if (!ok) {
                    fail("Invalid Content-Length from upstream");
                    return false;
                }
                has_length = true;
                content_length_ = n;
            } else if (lk == "transfer-encoding") {
                auto codings = lower_tokens(value);
                chunked = !codings.empty() && codings.back() == "chunked";
                continue; // The client connection frames the body itself.
            } else if (lk == "connection") {
                connection_tokens = lower_tokens(value);
                continue;
            }
            fields.emplace_back(std::move(key), std::move(value));
        }
        if (contains(connection_tokens, "close")) keep_alive_ = false;

        for (auto& [k, v] : fields) {
            std::string lk = to_lower(k);
            if (lk == "keep-alive" || lk == "proxy-connection" || lk == "te" || lk == "trailer" ||
                contains(connection_tokens, lk)) {
                continue;
            }
            if (lk == "upgrade" && status != 101) continue;
            res.headers[k] = v;
        }

        if (status == 101) {
            is_websocket_ = true;
            keep_alive_ = false;
            headers_sent_ = true;
            writer_->send_headers(res);
            start_websocket_passthrough();
            return true;
        }

        if (is_head_ || status == 204 || status == 304 || (status >= 100 && status < 200)) {
            body_mode_ = BodyMode::None;
        } else if (chunked) {
            body_mode_ = BodyMode::Chunked;
            res.headers.erase("Content-Length");
        } else if (has_length) {
            body_mode_ = BodyMode::ContentLength;
        } else {
            body_mode_ = BodyMode::CloseDelimited;
            keep_alive_ = false;
        }

        headers_sent_ = true;
        writer_->send_headers(res);
        if (body_mode_ == BodyMode::ContentLength && content_length_ == 0) {
            complete();
        }
        return true;
    }

    void start_websocket_passthrough() {
        auto self = shared_from_this();
        writer_->upgrade_to_raw_stream(
            [self](std::string_view data) {
                if (self->upstream_.use_tls) {
                    SSL_write(self->ssl_, data.data(), static_cast<int>(data.size()));
                    self->flush_tls_writes(nullptr);
                } else {
                    auto buf = std::make_shared<std::vector<char>>(data.begin(), data.end());
                    self->proactor_.async_write(self->fd_, buf->data(), buf->size(), [buf](ssize_t) {});
                }
            },
            [self]() {
                if (self->fd_ != -1) {
                    self->proactor_.remove(self->fd_);
                    network::close_socket(self->fd_);
                    self->fd_ = -1;
                }
            }
        );
    }

    enum class BodyMode { None, ContentLength, Chunked, CloseDelimited };

    network::Proactor& proactor_;
    std::shared_ptr<http::ResponseWriter> writer_;
    UpstreamConfig upstream_;
    detail::ProxiedRequest request_;
    bool is_head_{false};

    int fd_{-1};
    SSL* ssl_{nullptr};
    BIO* rbio_{nullptr};
    BIO* wbio_{nullptr};
    bool is_handshake_complete_{false};

    std::string write_buf_;
    char read_buf_[16384];

    std::string response_accumulator_;
    std::string leftover_;
    bool headers_sent_{false};
    bool is_reused_{false};
    bool keep_alive_{false};
    bool is_websocket_{false};
    bool finished_{false};
    BodyMode body_mode_{BodyMode::CloseDelimited};
    size_t content_length_{0};
    size_t body_bytes_read_{0};
    detail::ChunkedDecoder chunk_decoder_;
};

std::shared_ptr<network::ClientTlsContext> make_tls(bool needed, bool verify, const std::string& ca_file) {
    if (!needed) return nullptr;
    return std::make_shared<network::ClientTlsContext>(verify, ca_file);
}

} // namespace

routing::Middleware proxy(ProxyOptions options) {
    bool use_tls = options.use_tls || options.target_port == 443;
    UpstreamConfig upstream{options.target_host, options.target_port, use_tls, options.strip_prefix,
                            options.trust_forwarded_headers, make_tls(use_tls, options.verify_tls, options.ca_file)};
    return [upstream](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        auto proxy_req = std::make_shared<ProxyRequest>(writer->proactor(), writer, upstream,
                                                        detail::snapshot_request(request));
        proxy_req->start();
        return false;
    };
}

routing::Middleware proxy(const std::string& target_host, int target_port) {
    ProxyOptions opts;
    opts.target_host = target_host;
    opts.target_port = target_port;
    return proxy(opts);
}

routing::Middleware load_balancer(LoadBalancerOptions options) {
    if (options.nodes.empty()) {
        throw std::runtime_error("Load balancer requires at least one target node");
    }

    std::vector<UpstreamConfig> upstreams;
    std::shared_ptr<network::ClientTlsContext> shared_tls;
    for (const auto& node : options.nodes) {
        bool use_tls = node.use_tls || node.port == 443;
        if (use_tls && !shared_tls) shared_tls = make_tls(true, options.verify_tls, options.ca_file);
        upstreams.push_back({node.host, node.port, use_tls, options.strip_prefix,
                             options.trust_forwarded_headers, use_tls ? shared_tls : nullptr});
    }

    auto current_node = std::make_shared<std::atomic<size_t>>(0);

    return [upstreams, current_node](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        size_t idx = current_node->fetch_add(1, std::memory_order_relaxed) % upstreams.size();
        auto proxy_req = std::make_shared<ProxyRequest>(writer->proactor(), writer, upstreams[idx],
                                                        detail::snapshot_request(request));
        proxy_req->start();
        return false;
    };
}

routing::Middleware load_balancer(const std::vector<TargetNode>& nodes) {
    LoadBalancerOptions opts;
    opts.nodes = nodes;
    return load_balancer(opts);
}

} // namespace middleware
