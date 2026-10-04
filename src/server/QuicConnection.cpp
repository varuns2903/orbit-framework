#include <orbit/server/QuicConnection.hpp>
#include <orbit/utils/Logger.hpp>
#include <openssl/rand.h>
#include <cstdlib>
#include <orbit/server/QuicConnectionManager.hpp>
#include <orbit/server/QuicHttp3Session.hpp>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <utility>

namespace server {

static void my_ngtcp2_log_printf(void* user_data, const char* fmt, ...) {
    (void)user_data;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

QuicConnection::QuicConnection(QuicConnectionManager& manager, const ngtcp2_cid& client_dcid, const ngtcp2_cid& client_scid, const ngtcp2_cid& server_scid, const sockaddr_in& remote_addr, SSL_CTX* ssl_ctx)
    : manager_(manager), remote_addr_(remote_addr) {
    memset(&local_addr_, 0, sizeof(local_addr_));
    local_addr_.sin_family = AF_INET;
    local_addr_.sin_port = htons(8080); // Just a generic local port for now
    local_addr_.sin_addr.s_addr = INADDR_ANY;

    if (!init_ssl(ssl_ctx)) {
        throw std::runtime_error("Failed to initialize SSL for QUIC connection");
    }

    ngtcp2_callbacks callbacks{};
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.handshake_completed = on_handshake_completed;
    callbacks.recv_stream_data = on_recv_stream_data;
    callbacks.rand = rand_cb;
    callbacks.get_path_challenge_data = get_path_challenge_data_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_new_connection_id = on_get_new_connection_id;
    callbacks.remove_connection_id = on_remove_connection_id;
    // Stream data must stay alive until the peer acknowledges it (ngtcp2 may
    // retransmit from it), and the HTTP/3 layer must hear about stream
    // lifecycle and flow-control changes.
    callbacks.acked_stream_data_offset = on_acked_stream_data_offset;
    callbacks.stream_close = on_stream_close;
    callbacks.stream_reset = on_stream_reset;
    callbacks.stream_stop_sending = on_stream_stop_sending;
    callbacks.extend_max_stream_data = on_extend_max_stream_data;
    callbacks.extend_max_remote_streams_bidi = on_extend_max_remote_streams_bidi;
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    // ngtcp2 traces every packet; only wire it up when debugging.
    if (utils::Logger::current_level <= utils::LogLevel::DEBUG) {
        settings.log_printf = my_ngtcp2_log_printf;
    }
    settings.initial_ts = get_timestamp();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 65535;
    params.initial_max_stream_data_bidi_remote = 65535;
    params.initial_max_stream_data_uni = 65535;
    params.initial_max_data = 128 * 1024;
    params.initial_max_streams_bidi = 100;
    params.initial_max_streams_uni = 100;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    
    // Server must set original_dcid to the client's DCID from the initial packet
    params.original_dcid = client_dcid;
    params.original_dcid_present = 1;

    ngtcp2_path path = {
        { (sockaddr*)&local_addr_, sizeof(local_addr_) }, // local
        { (sockaddr*)&remote_addr_, sizeof(remote_addr_) }, // remote
        nullptr // user_data
    };

    int rv = ngtcp2_conn_server_new(&conn_, &client_scid, &server_scid, &path, NGTCP2_PROTO_VER_V1, &callbacks, &settings, &params, nullptr, this);
    if (rv != 0) {
        throw std::runtime_error("Failed to create ngtcp2 connection: " + std::string(ngtcp2_strerror(rv)));
    }
    
#if defined(USE_NGTCP2_CRYPTO_OSSL)
    ngtcp2_conn_set_tls_native_handle(conn_, tls_ctx_);
#elif defined(USE_NGTCP2_CRYPTO_QUICTLS)
    ngtcp2_conn_set_tls_native_handle(conn_, ssl_);
#endif
}

QuicConnection::~QuicConnection() {
    if (conn_) ngtcp2_conn_del(conn_);
#if defined(USE_NGTCP2_CRYPTO_OSSL)
    if (tls_ctx_) ngtcp2_crypto_ossl_ctx_del(tls_ctx_);
#endif
    if (ssl_) SSL_free(ssl_);
}

bool QuicConnection::init_ssl(SSL_CTX* ssl_ctx) {
    ssl_ = SSL_new(ssl_ctx);
    if (!ssl_) return false;
    
#if defined(USE_NGTCP2_CRYPTO_OSSL)
    if (ngtcp2_crypto_ossl_ctx_new(&tls_ctx_, ssl_) != 0) {
        return false;
    }
#endif
    
    conn_ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref) {
        auto* self = static_cast<QuicConnection*>(ref->user_data);
        return self->conn_;
    };
    conn_ref_.user_data = this;
    
    SSL_set_app_data(ssl_, &conn_ref_);
    
#if defined(USE_NGTCP2_CRYPTO_OSSL)
    if (ngtcp2_crypto_ossl_configure_server_session(ssl_) != 0) {
        return false;
    }
#endif
    
    SSL_set_accept_state(ssl_);
    // SSL_set_quic_early_data_enabled(ssl_, 1); // OpenSSL 3.2+ only or disabled for now
    
    return true;
}

void QuicConnection::process_packet(const uint8_t* data, size_t datalen, const sockaddr_in& remote_addr) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (closed_ || closing_) return; // nothing more to say on a closing connection
    LOG_DEBUG("QUIC: process_packet datalen=" << datalen);
    (void)remote_addr;
    ngtcp2_path path = {
        { (sockaddr*)&local_addr_, sizeof(local_addr_) },
        { (sockaddr*)&remote_addr_, sizeof(remote_addr_) },
        this
    };
    
    ngtcp2_pkt_info pi{};
    int rv = ngtcp2_conn_read_pkt(conn_, &path, &pi, data, datalen, get_timestamp()); 
    if (rv != 0) {
        LOG_DEBUG("ngtcp2_conn_read_pkt failed: " << ngtcp2_strerror(rv));
        if (rv == NGTCP2_ERR_DRAINING) {
            enter_draining(); // the peer closed the connection
        } else if (rv == NGTCP2_ERR_DROP_CONN) {
            closed_ = true;   // ngtcp2 says: forget it without a word
        } else {
            start_closing(rv);
        }
        return;
    }
    send_pending_data();
}

void QuicConnection::handle_expiry() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (closed_) return;
    auto now = std::chrono::steady_clock::now();
    if (closing_) {
        if (now >= close_deadline_) closed_ = true;
        return;
    }
    if (ngtcp2_conn_get_expiry(conn_) > get_timestamp()) return; // nothing due yet
    int rv = ngtcp2_conn_handle_expiry(conn_, get_timestamp());
    if (rv == NGTCP2_ERR_IDLE_CLOSE) {
        closed_ = true; // idle timeout: close silently (RFC 9000 section 10.1)
        return;
    }
    if (rv != 0) {
        start_closing(rv);
        return;
    }
    send_pending_data();
}

int64_t QuicConnection::ns_until_expiry() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (closed_) return -1;
    if (closing_) {
        auto left = close_deadline_ - std::chrono::steady_clock::now();
        return std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count());
    }
    ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn_);
    if (expiry == UINT64_MAX) return -1;
    ngtcp2_tstamp now = get_timestamp();
    return expiry <= now ? 0 : static_cast<int64_t>(expiry - now);
}

// Sends one CONNECTION_CLOSE, then keeps the state for three PTOs so late
// packets are absorbed (RFC 9000 section 10.2).
void QuicConnection::start_closing(int liberr) {
    if (closing_ || closed_) return;
    closing_ = true;
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};
    uint8_t buf[1280];
    ngtcp2_ccerr ccerr;
    ngtcp2_ccerr_default(&ccerr);
    if (liberr != 0) ngtcp2_ccerr_set_liberr(&ccerr, liberr, nullptr, 0);
    ngtcp2_ssize n = ngtcp2_conn_write_connection_close(conn_, &ps.path, &pi, buf, sizeof(buf), &ccerr, get_timestamp());
    if (n > 0) {
        manager_.send_packet(buf, static_cast<size_t>(n), ps.path.remote.addr, ps.path.remote.addrlen);
    }
    close_deadline_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(3 * ngtcp2_conn_get_pto(conn_));
}

void QuicConnection::enter_draining() {
    if (closing_ || closed_) return;
    closing_ = true; // draining: send nothing at all (RFC 9000 section 10.2.2)
    close_deadline_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(3 * ngtcp2_conn_get_pto(conn_));
}

std::vector<ngtcp2_cid> QuicConnection::take_issued_cids() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return std::exchange(issued_cids_, {});
}

std::vector<ngtcp2_cid> QuicConnection::take_retired_cids() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return std::exchange(retired_cids_, {});
}

void QuicConnection::extend_stream_credit(int64_t stream_id, uint64_t consumed) {
    if (consumed == 0) return;
    ngtcp2_conn_extend_max_stream_offset(conn_, stream_id, consumed);
    ngtcp2_conn_extend_max_offset(conn_, consumed);
}

ngtcp2_tstamp QuicConnection::get_timestamp() const {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void QuicConnection::send_pending_data() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (closed_ || closing_) return;
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi;
    uint8_t outbuf[1280]; // standard max UDP payload
    
    bool writing_more = false;
    
    for (;;) {
        ngtcp2_ssize ndatalen = 0;
        int64_t stream_id = -1;
        int fin = 0;
        nghttp3_vec vec[16];
        nghttp3_ssize veccnt = 0;
        
        if (h3_session_) {
            veccnt = nghttp3_conn_writev_stream(h3_session_->get_conn(), &stream_id, &fin, vec, 16);
            if (veccnt < 0) {
                LOG_ERROR("nghttp3_conn_writev_stream failed: " << nghttp3_strerror(static_cast<int>(veccnt)));
                start_closing(NGTCP2_ERR_INTERNAL);
                return;
            }
        }
        
        if (h3_session_ && (veccnt > 0 || stream_id != -1 || writing_more)) {
            ngtcp2_vec quic_vec[16];
            for (nghttp3_ssize i = 0; i < veccnt; ++i) {
                quic_vec[i].base = (uint8_t*)vec[i].base;
                quic_vec[i].len = vec[i].len;
            }
            
            uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
            if (fin) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
            if (stream_id == -1) flags = 0; // stop coalescing
            
            ngtcp2_ssize datalen_written = 0;
            ndatalen = ngtcp2_conn_writev_stream(conn_, &ps.path, &pi, outbuf, sizeof(outbuf),
                                                 &datalen_written, flags, stream_id,
                                                 quic_vec, veccnt, get_timestamp());
            
            LOG_DEBUG("QUIC: writev_stream stream=" << stream_id << " veccnt=" << veccnt << " flags=" << flags << " returned ndatalen=" << ndatalen << " datalen_written=" << datalen_written);
            
            if (datalen_written > 0) {
                // Written, not acknowledged: nghttp3 must keep the data until
                // on_acked_stream_data_offset, as ngtcp2 may retransmit it.
                nghttp3_conn_add_write_offset(h3_session_->get_conn(), stream_id, datalen_written);
            }
            
            if (ndatalen == NGTCP2_ERR_STREAM_DATA_BLOCKED || ndatalen == NGTCP2_ERR_STREAM_SHUT_WR || ndatalen == NGTCP2_ERR_STREAM_NOT_FOUND) {
                nghttp3_conn_block_stream(h3_session_->get_conn(), stream_id);
                writing_more = true;
                continue;
            } else if (ndatalen == NGTCP2_ERR_WRITE_MORE) {
                writing_more = true;
                continue;
            } else {
                writing_more = false;
            }
        } else {
            ndatalen = ngtcp2_conn_write_pkt(conn_, &ps.path, &pi, outbuf, sizeof(outbuf), get_timestamp());
            if (ndatalen > 0) {
                LOG_DEBUG("QUIC: write_pkt returned ndatalen=" << ndatalen);
            }
        }
        
        if (ndatalen < 0) {
            start_closing(static_cast<int>(ndatalen)); // a fatal ngtcp2 error
            break;
        }
        if (ndatalen == 0) {
            break;
        }
        
        LOG_DEBUG("QUIC: sending UDP packet of size " << ndatalen);
        manager_.send_packet(outbuf, static_cast<size_t>(ndatalen), ps.path.remote.addr, ps.path.remote.addrlen);
    }
}

int QuicConnection::on_handshake_completed(ngtcp2_conn *conn, void *user_data) {
    auto self = static_cast<QuicConnection*>(user_data);
    LOG_DEBUG("QUIC Handshake Completed!");
    
    self->h3_session_ = std::make_unique<QuicHttp3Session>(*self);
    self->h3_session_->init();
    
    int rv = 0;
    int64_t ctrl_id = -1, qenc_id = -1, qdec_id = -1;
    
    rv = ngtcp2_conn_open_uni_stream(conn, &ctrl_id, nullptr);
    if (rv != 0) LOG_ERROR("QUIC: Failed to open control stream: " << ngtcp2_strerror(rv));
    
    rv = ngtcp2_conn_open_uni_stream(conn, &qenc_id, nullptr);
    if (rv != 0) LOG_ERROR("QUIC: Failed to open qenc stream: " << ngtcp2_strerror(rv));
    
    rv = ngtcp2_conn_open_uni_stream(conn, &qdec_id, nullptr);
    if (rv != 0) LOG_ERROR("QUIC: Failed to open qdec stream: " << ngtcp2_strerror(rv));

    rv = nghttp3_conn_bind_control_stream(self->h3_session_->get_conn(), ctrl_id);
    if (rv != 0) LOG_ERROR("QUIC: Failed to bind control stream: " << nghttp3_strerror(rv));
    
    rv = nghttp3_conn_bind_qpack_streams(self->h3_session_->get_conn(), qenc_id, qdec_id);
    if (rv != 0) LOG_ERROR("QUIC: Failed to bind qpack streams: " << nghttp3_strerror(rv));
    
    self->send_pending_data();
    
    return 0;
}

int QuicConnection::on_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *user_data, void *stream_user_data) {
    auto self = static_cast<QuicConnection*>(user_data);
    bool fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN);
    LOG_DEBUG("QUIC: on_recv_stream_data id=" << stream_id << " len=" << datalen << " fin=" << fin);
    if (self->h3_session_) {
        auto consumed = self->h3_session_->process_stream_data(stream_id, data, datalen, fin);
        if (consumed < 0) return NGTCP2_ERR_CALLBACK_FAILURE;
        // Give back flow-control credit for what nghttp3 consumed; otherwise
        // a stream (and the connection) stalls once its window is used up.
        self->extend_stream_credit(stream_id, static_cast<uint64_t>(consumed));
    }
    return 0;
}

int QuicConnection::on_acked_stream_data_offset(ngtcp2_conn *, int64_t stream_id, uint64_t, uint64_t datalen, void *user_data, void *) {
    auto self = static_cast<QuicConnection*>(user_data);
    if (self->h3_session_ && nghttp3_conn_add_ack_offset(self->h3_session_->get_conn(), stream_id, datalen) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicConnection::on_stream_close(ngtcp2_conn *, uint32_t flags, int64_t stream_id, uint64_t app_error_code, void *user_data, void *) {
    auto self = static_cast<QuicConnection*>(user_data);
    if (!self->h3_session_) return 0;
    if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) app_error_code = NGHTTP3_H3_NO_ERROR;
    int rv = nghttp3_conn_close_stream(self->h3_session_->get_conn(), stream_id, app_error_code);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

int QuicConnection::on_stream_reset(ngtcp2_conn *, int64_t stream_id, uint64_t, uint64_t, void *user_data, void *) {
    auto self = static_cast<QuicConnection*>(user_data);
    if (self->h3_session_ && nghttp3_conn_shutdown_stream_read(self->h3_session_->get_conn(), stream_id) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicConnection::on_stream_stop_sending(ngtcp2_conn *, int64_t stream_id, uint64_t, void *user_data, void *) {
    auto self = static_cast<QuicConnection*>(user_data);
    if (self->h3_session_ && nghttp3_conn_shutdown_stream_read(self->h3_session_->get_conn(), stream_id) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicConnection::on_extend_max_stream_data(ngtcp2_conn *, int64_t stream_id, uint64_t, void *user_data, void *) {
    auto self = static_cast<QuicConnection*>(user_data);
    // The peer granted more credit: a stream nghttp3 blocked may continue.
    if (self->h3_session_ && nghttp3_conn_unblock_stream(self->h3_session_->get_conn(), stream_id) != 0) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicConnection::on_extend_max_remote_streams_bidi(ngtcp2_conn *, uint64_t max_streams, void *user_data) {
    auto self = static_cast<QuicConnection*>(user_data);
    if (self->h3_session_) nghttp3_conn_set_max_client_streams_bidi(self->h3_session_->get_conn(), max_streams);
    return 0;
}

int QuicConnection::on_remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *user_data) {
    auto self = static_cast<QuicConnection*>(user_data);
    self->retired_cids_.push_back(*cid);
    return 0;
}

// Issues a new connection ID and remembers it, so the manager routes the
// peer's packets for it here (clients may switch to it at any time).
int QuicConnection::on_get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data) {
    int rv = get_new_connection_id_cb(conn, cid, token, cidlen, user_data);
    if (rv == 0) static_cast<QuicConnection*>(user_data)->issued_cids_.push_back(*cid);
    return rv;
}

namespace detail {

bool quic_random_bytes(uint8_t* dest, size_t len) {
    return len == 0 || RAND_bytes(dest, static_cast<int>(len)) == 1;
}

} // namespace detail

void QuicConnection::rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx) {
    (void)rand_ctx;
    if (!detail::quic_random_bytes(dest, destlen)) {
        // ngtcp2 gives this callback no way to fail, and continuing with
        // predictable bytes would be worse than stopping.
        LOG_ERROR("RAND_bytes failed in QUIC rand callback; aborting");
        std::abort();
    }
}

int QuicConnection::get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data) {
    (void)conn; (void)user_data;
    cid->datalen = cidlen;
    if (!detail::quic_random_bytes(cid->data, cidlen) ||
        !detail::quic_random_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN)) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int QuicConnection::get_path_challenge_data_cb(ngtcp2_conn *conn, uint8_t *data, void *user_data) {
    (void)conn; (void)user_data;
    if (!detail::quic_random_bytes(data, NGTCP2_PATH_CHALLENGE_DATALEN)) {
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

} // namespace server
