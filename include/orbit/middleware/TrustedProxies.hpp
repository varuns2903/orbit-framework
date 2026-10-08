#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/routing/Router.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::middleware {

/**
 * @brief An IPv4 or IPv6 network ("10.0.0.0/8", "::1", "fd00::/8").
 *        IPv4 addresses are stored IPv4-mapped, so one comparison handles both.
 */
class IpNetwork {
public:
    /// Parses an address or CIDR; std::nullopt if malformed.
    static std::optional<IpNetwork> parse(std::string_view text);
    /// True if `ip` (a literal address) is inside this network.
    bool contains(std::string_view ip) const;

private:
    std::array<uint8_t, 16> address_{};
    int prefix_bits_ = 128;
};

/**
 * @brief Options for trusted_proxies().
 */
struct TrustedProxyOptions {
    /// Proxies whose forwarding headers are believed: addresses or CIDRs,
    /// e.g. {"127.0.0.1", "10.0.0.0/8", "::1"}.
    std::vector<std::string> proxies;
    /// "X-Forwarded-For" (default) or "Forwarded" (RFC 7239).
    std::string header = "X-Forwarded-For";
};

/**
 * @ingroup middlewares
 * @brief Sets req.client_ip from forwarding headers, but only for requests that
 *        arrive from a trusted proxy.
 *
 * The header is read right to left, skipping trusted proxies; the first
 * address that is not one of them is the client. Requests from anyone else
 * keep their socket address, so clients cannot spoof their IP. The socket
 * address stays available as req.peer_ip.
 *
 * Register it before middleware that uses client_ip (rate limiting, logging).
 *
 * @throws std::invalid_argument if an entry in `proxies` is malformed.
 */
routing::Middleware trusted_proxies(TrustedProxyOptions options);

} // namespace middleware
