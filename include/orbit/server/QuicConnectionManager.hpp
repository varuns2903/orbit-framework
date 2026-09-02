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

namespace server {

class QuicConnection; // Forward declaration

struct QuicConnectionIdHash {
    std::size_t operator()(const ngtcp2_cid& cid) const {
        std::size_t hash = 0;
        for (size_t i = 0; i < cid.datalen; ++i) {
            hash ^= (static_cast<std::size_t>(cid.data[i]) << (i % 8));
        }
        return hash;
    }
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

private:    // Periodically handle QUIC timers for all connections
    void handle_timers();

private:
    network::UdpSocket& socket_;
    SSL_CTX* ssl_ctx_{nullptr};
    std::unordered_map<ngtcp2_cid, std::shared_ptr<QuicConnection>, QuicConnectionIdHash, QuicConnectionIdEqual> connections_;
};

} // namespace server
