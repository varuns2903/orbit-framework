#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#if defined(USE_NGTCP2_CRYPTO_OSSL)
#include <ngtcp2/ngtcp2_crypto_ossl.h>
#define NGTCP2_CRYPTO_CONFIGURE_SERVER_CONTEXT(ssl_ctx) (0)
#elif defined(USE_NGTCP2_CRYPTO_QUICTLS)
#include <ngtcp2/ngtcp2_crypto_quictls.h>
#define NGTCP2_CRYPTO_CONFIGURE_SERVER_CONTEXT(ssl_ctx) ngtcp2_crypto_quictls_configure_server_context(ssl_ctx)
#else
#error "No supported ngtcp2 crypto backend found"
#endif

#include <openssl/ssl.h>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <string>
#include <orbit/network/PlatformSocket.hpp>

namespace server {

namespace detail {

/**
 * @brief Fills @p dest with bytes from the OpenSSL CSPRNG. QUIC connection
 *        IDs, stateless reset tokens and path challenges must be
 *        unpredictable (RFC 9000 sections 5.1, 8.2 and 10.3).
 * @return False if the CSPRNG failed.
 */
bool quic_random_bytes(uint8_t* dest, size_t len);

} // namespace detail


class QuicConnectionManager;
class QuicHttp3Session;

/**
 * @brief Represents a single QUIC connection.
 */
class QuicConnection : public std::enable_shared_from_this<QuicConnection> {
public:
    /**
     * @brief Constructs a QuicConnection.
     * @param manager The QUIC connection manager.
     * @param client_dcid Client destination connection ID.
     * @param client_scid Client source connection ID.
     * @param server_scid Server source connection ID.
     * @param remote_addr The remote address.
     * @param ssl_ctx The SSL context.
     */
    QuicConnection(QuicConnectionManager& manager, const ngtcp2_cid& client_dcid, const ngtcp2_cid& client_scid, const ngtcp2_cid& server_scid, const sockaddr_in& remote_addr, SSL_CTX* ssl_ctx);
    ~QuicConnection();

    /**
     * @brief Processes an incoming QUIC packet.
     * @param data The packet data.
     * @param datalen The length of the data.
     * @param remote_addr The remote address.
     */
    void process_packet(const uint8_t* data, size_t datalen, const sockaddr_in& remote_addr);
    
    /**
     * @brief Runs QUIC timers that are due (loss detection, delayed ACKs,
     *        idle timeout, the closing period). Event-loop thread.
     */
    void handle_expiry();

    /// Nanoseconds until the next timer is due (0 if overdue), or -1 if none.
    int64_t ns_until_expiry() const;

    /// True once the connection has ended and can be forgotten.
    bool is_closed() const { return closed_; }

    /// Connection IDs this connection issued or retired since the last call;
    /// the manager routes packets for them to this connection.
    std::vector<ngtcp2_cid> take_issued_cids();
    std::vector<ngtcp2_cid> take_retired_cids();

    /// Guards ngtcp2/nghttp3 state: packets are processed on the event loop,
    /// responses are submitted from worker threads.
    std::recursive_mutex& mutex() { return mutex_; }

    /// Returns flow-control credit for `consumed` bytes the application has
    /// taken off `stream_id`. Caller holds mutex().
    void extend_stream_credit(int64_t stream_id, uint64_t consumed);
    
    /**
     * @brief Sends any pending data for the connection.
     */
    void send_pending_data();

    QuicConnectionManager& manager() { return manager_; }
    /// The HTTP/3 session, once the handshake has completed. Caller holds mutex().
    QuicHttp3Session* http3_session() { return h3_session_.get(); }
    /// The peer's IP address as text.
    std::string remote_ip() const;

private:
    QuicConnectionManager& manager_;
    sockaddr_in local_addr_;
    sockaddr_in remote_addr_;
    ngtcp2_conn* conn_{nullptr};
    SSL* ssl_ = nullptr;
#if defined(USE_NGTCP2_CRYPTO_OSSL)
    ngtcp2_crypto_ossl_ctx* tls_ctx_ = nullptr;
#endif
    ngtcp2_crypto_conn_ref conn_ref_{};
    
    std::unique_ptr<QuicHttp3Session> h3_session_;

    mutable std::recursive_mutex mutex_;
    bool closed_ = false;
    // Closing or draining: no more packets are sent (except one
    // CONNECTION_CLOSE), and the state is dropped at close_deadline_.
    bool closing_ = false;
    std::chrono::steady_clock::time_point close_deadline_{};
    std::vector<ngtcp2_cid> issued_cids_;
    std::vector<ngtcp2_cid> retired_cids_;

    void start_closing(int liberr);
    void enter_draining();

    ngtcp2_tstamp get_timestamp() const;
    bool init_ssl(SSL_CTX* ssl_ctx);

    static int on_handshake_completed(ngtcp2_conn *conn, void *user_data);
    static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *rand_ctx);
    static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data);
    static int get_path_challenge_data_cb(ngtcp2_conn *conn, uint8_t *data, void *user_data);
    static int on_recv_stream_data(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t offset, const uint8_t *data, size_t datalen, void *user_data, void *stream_user_data);
    static int on_get_new_connection_id(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token, size_t cidlen, void *user_data);
    static int on_remove_connection_id(ngtcp2_conn *conn, const ngtcp2_cid *cid, void *user_data);
    static int on_acked_stream_data_offset(ngtcp2_conn *conn, int64_t stream_id, uint64_t offset, uint64_t datalen, void *user_data, void *stream_user_data);
    static int on_stream_close(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id, uint64_t app_error_code, void *user_data, void *stream_user_data);
    static int on_stream_reset(ngtcp2_conn *conn, int64_t stream_id, uint64_t final_size, uint64_t app_error_code, void *user_data, void *stream_user_data);
    static int on_stream_stop_sending(ngtcp2_conn *conn, int64_t stream_id, uint64_t app_error_code, void *user_data, void *stream_user_data);
    static int on_extend_max_stream_data(ngtcp2_conn *conn, int64_t stream_id, uint64_t max_data, void *user_data, void *stream_user_data);
    static int on_extend_max_remote_streams_bidi(ngtcp2_conn *conn, uint64_t max_streams, void *user_data);
};

} // namespace server
