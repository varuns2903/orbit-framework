#include <orbit/server/QuicConnectionManager.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/server/QuicConnection.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <iostream>
#include <cstring>

namespace server {

QuicConnectionManager::QuicConnectionManager(network::UdpSocket& socket, SSL_CTX* ssl_ctx)
    : socket_(socket), ssl_ctx_(ssl_ctx) {
    if (NGTCP2_CRYPTO_CONFIGURE_SERVER_CONTEXT(ssl_ctx_) != 0) {
        LOG_ERROR("Failed to configure QUIC server context");
    }
}

QuicConnectionManager::~QuicConnectionManager() {
}

void QuicConnectionManager::on_packet_received(const uint8_t* data, size_t datalen, const sockaddr_in& sender_addr) {
    if (datalen == 0) return;
    (void)sender_addr;

    ngtcp2_version_cid ver_cid;
    
    // Decode QUIC header to extract the Destination Connection ID (DCID)
    // short_dcidlen must match the length of the SCID we generate (8 bytes).
    int rv = ngtcp2_pkt_decode_version_cid(&ver_cid, data, datalen, 8);
    if (rv < 0) {
        LOG_DEBUG("Failed to decode QUIC packet version and CID: " << ngtcp2_strerror(rv));
        return;
    }

    ngtcp2_cid dcid_struct;
    ngtcp2_cid_init(&dcid_struct, ver_cid.dcid, ver_cid.dcidlen);

    LOG_DEBUG("QCM: Incoming packet DCID (len=" << dcid_struct.datalen << ")");

    auto it = connections_.find(dcid_struct);
    if (it != connections_.end()) {
        LOG_DEBUG("QCM: Found existing connection for DCID (len=" << dcid_struct.datalen << ")");
        it->second->process_packet(data, datalen, sender_addr);
    } else {
        if (ver_cid.version == 0) {
            LOG_DEBUG("QCM: Dropping version negotiation packet");
            return;
        }
        
        LOG_DEBUG("QCM: Received QUIC packet for unknown connection, creating new instance (DCID len=" << dcid_struct.datalen << ")");
        
        // Use client's SCID as our DCID, and generate a new random SCID for the server
        ngtcp2_cid scid_struct;
        scid_struct.datalen = 8;
        if (!detail::quic_random_bytes(scid_struct.data, scid_struct.datalen)) {
            return; // Drop the packet rather than issue a predictable connection ID
        }
        

        ngtcp2_cid parsed_scid;
        ngtcp2_cid_init(&parsed_scid, ver_cid.scid, ver_cid.scidlen);

        auto conn = std::make_shared<QuicConnection>(*this, dcid_struct, parsed_scid, scid_struct, sender_addr, ssl_ctx_);
        connections_[dcid_struct] = conn;
        connections_[scid_struct] = conn; // Also map by our SCID for return packets
        utils::PrometheusRegistry::get_instance().inc_gauge("orbit_active_connections", "type=\"quic\"");
        conn->process_packet(data, datalen, sender_addr);
    }
}

void QuicConnectionManager::handle_timers() {
    // Iterate over connections and handle expiry
    for (auto& [cid, conn] : connections_) {
        conn->handle_expiry();
    }
}

void QuicConnectionManager::send_packet(const uint8_t* data, size_t datalen, const sockaddr* remote_addr, socklen_t remote_addrlen) {
    (void)remote_addrlen;
    if (remote_addr->sa_family == AF_INET) {
        socket_.send_to(reinterpret_cast<const char*>(data), datalen, *reinterpret_cast<const sockaddr_in*>(remote_addr));
    }
}

} // namespace server
