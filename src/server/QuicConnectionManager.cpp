#include <orbit/server/QuicConnectionManager.hpp>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <orbit/utils/Logger.hpp>
#include <orbit/server/QuicConnection.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <iostream>
#include <cstring>

namespace server {

namespace quic::detail {

bool acceptable_first_packet(const uint8_t* data, size_t datalen) {
    ngtcp2_pkt_hd hd;
    return ngtcp2_accept(&hd, data, datalen) == 0;
}

} // namespace quic::detail

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
        auto conn = it->second; // keep it alive while it may be removed
        conn->process_packet(data, datalen, sender_addr);
        sync_connection(conn);
    } else {
        if (!quic::detail::acceptable_first_packet(data, datalen)) {
            LOG_DEBUG("QCM: Dropping packet for unknown connection that cannot start one");
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
        sync_connection(conn);
    }
}

std::size_t QuicConnectionIdHash::operator()(const ngtcp2_cid& cid) const {
    static const std::string key = [] {
        std::string k(16, '\0');
        detail::quic_random_bytes(reinterpret_cast<uint8_t*>(&k[0]), k.size());
        return k;
    }();
    std::string material = key;
    material.append(reinterpret_cast<const char*>(cid.data), cid.datalen);
    return std::hash<std::string>{}(material);
}

void QuicConnectionManager::sync_connection(const std::shared_ptr<QuicConnection>& conn) {
    for (const auto& cid : conn->take_issued_cids()) connections_[cid] = conn;
    for (const auto& cid : conn->take_retired_cids()) {
        auto it = connections_.find(cid);
        if (it != connections_.end() && it->second == conn) connections_.erase(it);
    }
    if (conn->is_closed()) {
        for (auto it = connections_.begin(); it != connections_.end();) {
            if (it->second == conn) {
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
        LOG_DEBUG("QUIC: connection closed and removed");
        utils::PrometheusRegistry::get_instance().dec_gauge("orbit_active_connections", "type=\"quic\"");
    }
}

size_t QuicConnectionManager::unique_connections() const {
    std::unordered_set<QuicConnection*> seen;
    for (const auto& [cid, conn] : connections_) seen.insert(conn.get());
    return seen.size();
}

void QuicConnectionManager::handle_timers() {
    // Each connection appears once per connection ID; visit it once.
    std::vector<std::shared_ptr<QuicConnection>> unique;
    std::unordered_set<QuicConnection*> seen;
    for (auto& [cid, conn] : connections_) {
        if (seen.insert(conn.get()).second) unique.push_back(conn);
    }
    for (auto& conn : unique) {
        conn->handle_expiry();
        sync_connection(conn);
    }
}

int QuicConnectionManager::next_timeout_ms() const {
    int64_t best = -1;
    std::unordered_set<QuicConnection*> seen;
    for (const auto& [cid, conn] : connections_) {
        if (!seen.insert(conn.get()).second) continue;
        int64_t ns = conn->ns_until_expiry();
        if (ns >= 0 && (best < 0 || ns < best)) best = ns;
    }
    if (best < 0) return -1;
    return static_cast<int>((best + 999999) / 1000000); // round up to whole ms
}

void QuicConnectionManager::send_packet(const uint8_t* data, size_t datalen, const sockaddr* remote_addr, socklen_t remote_addrlen) {
    (void)remote_addrlen;
    if (remote_addr->sa_family == AF_INET) {
        socket_.send_to(reinterpret_cast<const char*>(data), datalen, *reinterpret_cast<const sockaddr_in*>(remote_addr));
    }
}

} // namespace server
