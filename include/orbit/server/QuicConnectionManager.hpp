#pragma once
#include <orbit/server/QuicConnection.hpp>
#include <unordered_map>
#include <memory>
#include <openssl/ssl.h>
#include <vector>
#include <cstring>
#include <ngtcp2/ngtcp2.h>
#include <orbit/network/PlatformSocket.hpp>
#include <orbit/network/UdpSocket.hpp>

namespace routing { class Router; }
namespace concurrency { class ThreadPool; }
namespace network { class Proactor; }

namespace server {

/// What HTTP/3 requests are dispatched with; set by the event loop before
/// it runs.
struct Http3Context {
    const routing::Router* router = nullptr;
    concurrency::ThreadPool* thread_pool = nullptr;
    network::Proactor* proactor = nullptr;
    size_t max_body_size = 0;
};

class QuicConnection; // Forward declaration

namespace quic::detail {

/// True if a packet whose connection ID is unknown may open a connection:
/// a client Initial of at least 1200 bytes with a Destination Connection
/// ID of at least 8 bytes (RFC 9000 sections 7.2 and 14.1). Anything else
/// is dropped, so it cannot make the server allocate connection state.
bool acceptable_first_packet(const uint8_t* data, size_t datalen);

} // namespace quic::detail

/// Hashes connection IDs with a random per-process key. Clients choose the
/// IDs of their Initial packets, so an unkeyed hash would let them pile
/// entries into one bucket.
struct QuicConnectionIdHash {
    std::size_t operator()(const ngtcp2_cid& cid) const;
};

struct QuicConnectionIdEqual {
    bool operator()(const ngtcp2_cid& a, const ngtcp2_cid& b) const {
        if (a.datalen != b.datalen) return false;
        return std::memcmp(a.data, b.data, a.datalen) == 0;
    }
};

/**
 * @brief Manages QUIC connections.
 */
class QuicConnectionManager {
public:
    /**
     * @brief Constructs a QuicConnectionManager.
     * @param socket The UDP socket for QUIC.
     * @param ssl_ctx The SSL context for QUIC.
     */
    QuicConnectionManager(network::UdpSocket& socket, SSL_CTX* ssl_ctx);
    ~QuicConnectionManager();

    /**
     * @brief Processes an incoming UDP packet for QUIC.
     * @param data The packet data.
     * @param datalen The length of the data.
     * @param remote_addr The remote address of the sender.
     */
    void on_packet_received(const uint8_t* data, size_t datalen, const sockaddr_in& remote_addr);
    
    /**
     * @brief Sends a QUIC packet to a remote address.
     * @param data The packet data.
     * @param datalen The length of the data.
     * @param remote_addr The remote address.
     * @param remote_addrlen The length of the remote address struct.
     */
    void send_packet(const uint8_t* data, size_t datalen, const sockaddr* remote_addr, socklen_t remote_addrlen);

    /// Event-loop thread: runs due timers and forgets finished connections.
    void handle_timers();

    /// Milliseconds until a connection's next timer, or -1 if none.
    int next_timeout_ms() const;

    size_t connection_count() const { return unique_connections(); }

    void set_http_context(const Http3Context& ctx) { http_context_ = ctx; }
    const Http3Context& http_context() const { return http_context_; }

private:
    network::UdpSocket& socket_;
    SSL_CTX* ssl_ctx_{nullptr};
    Http3Context http_context_;
    std::unordered_map<ngtcp2_cid, std::shared_ptr<QuicConnection>, QuicConnectionIdHash, QuicConnectionIdEqual> connections_;

    // Routes newly issued IDs to their connection, drops retired ones, and
    // removes the connection entirely once it has closed.
    void sync_connection(const std::shared_ptr<QuicConnection>& conn);
    size_t unique_connections() const;
};

} // namespace server
