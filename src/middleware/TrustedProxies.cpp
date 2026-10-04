#include <orbit/middleware/TrustedProxies.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <cctype>
#include <cstring>
#include <stdexcept>

namespace middleware {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Parses an IPv4 or IPv6 literal into 16 bytes (IPv4 as ::ffff:a.b.c.d).
bool parse_ip(std::string_view text, std::array<uint8_t, 16>& out, bool& is_v4) {
    std::string s(text);
    in_addr v4{};
    if (inet_pton(AF_INET, s.c_str(), &v4) == 1) {
        out.fill(0);
        out[10] = 0xff;
        out[11] = 0xff;
        std::memcpy(out.data() + 12, &v4, 4);
        is_v4 = true;
        return true;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, s.c_str(), &v6) == 1) {
        std::memcpy(out.data(), &v6, 16);
        is_v4 = false;
        return true;
    }
    return false;
}

// "203.0.113.7", "203.0.113.7:4711", "[2001:db8::1]:4711", "\"[2001:db8::1]\"" -> bare address.
std::string_view bare_address(std::string_view v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (!v.empty() && v.front() == '[') {
        size_t close = v.find(']');
        return close == std::string_view::npos ? std::string_view{} : v.substr(1, close - 1);
    }
    // IPv4 with a port has exactly one colon; IPv6 has several.
    size_t colon = v.find(':');
    if (colon != std::string_view::npos && v.find(':', colon + 1) == std::string_view::npos) {
        return v.substr(0, colon);
    }
    return v;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Addresses in the header, in order (client first).
std::vector<std::string_view> forwarded_chain(std::string_view value, bool rfc7239) {
    std::vector<std::string_view> chain;
    while (!value.empty()) {
        size_t comma = value.find(',');
        std::string_view element = value.substr(0, comma);
        if (rfc7239) {
            // for=...;proto=...;by=...  -> the for= parameter
            std::string_view found;
            while (!element.empty()) {
                size_t semi = element.find(';');
                std::string_view pair = trim(element.substr(0, semi));
                size_t eq = pair.find('=');
                if (eq != std::string_view::npos && iequals(trim(pair.substr(0, eq)), "for")) {
                    found = pair.substr(eq + 1);
                }
                if (semi == std::string_view::npos) break;
                element.remove_prefix(semi + 1);
            }
            chain.push_back(bare_address(found));
        } else {
            chain.push_back(bare_address(element));
        }
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return chain;
}

} // namespace

std::optional<IpNetwork> IpNetwork::parse(std::string_view text) {
    text = trim(text);
    std::string_view addr = text;
    int bits = -1;
    size_t slash = text.find('/');
    if (slash != std::string_view::npos) {
        addr = text.substr(0, slash);
        std::string_view b = text.substr(slash + 1);
        if (b.empty() || b.size() > 3) return std::nullopt;
        bits = 0;
        for (char c : b) {
            if (c < '0' || c > '9') return std::nullopt;
            bits = bits * 10 + (c - '0');
        }
    }
    IpNetwork net;
    bool is_v4 = false;
    if (!parse_ip(addr, net.address_, is_v4)) return std::nullopt;
    if (bits < 0) bits = is_v4 ? 32 : 128;
    if (bits > (is_v4 ? 32 : 128)) return std::nullopt;
    net.prefix_bits_ = is_v4 ? bits + 96 : bits;
    return net;
}

bool IpNetwork::contains(std::string_view ip) const {
    std::array<uint8_t, 16> other{};
    bool is_v4 = false;
    if (!parse_ip(bare_address(ip), other, is_v4)) return false;
    int bits = prefix_bits_;
    for (size_t i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
        uint8_t mask = bits >= 8 ? 0xff : static_cast<uint8_t>(0xff << (8 - bits));
        if ((address_[i] & mask) != (other[i] & mask)) return false;
    }
    return true;
}

routing::Middleware trusted_proxies(TrustedProxyOptions options) {
    std::vector<IpNetwork> trusted;
    for (const auto& entry : options.proxies) {
        auto net = IpNetwork::parse(entry);
        if (!net) throw std::invalid_argument("trusted_proxies: not an IP address or CIDR: " + entry);
        trusted.push_back(*net);
    }
    const bool rfc7239 = iequals(options.header, "Forwarded");
    std::string header = options.header;

    return [trusted = std::move(trusted), header, rfc7239](http::HttpRequest& req,
                                                             std::shared_ptr<http::ResponseWriter>) -> bool {
        auto is_trusted = [&trusted](std::string_view ip) {
            for (const auto& net : trusted) {
                if (net.contains(ip)) return true;
            }
            return false;
        };
        if (req.peer_ip.empty()) req.peer_ip = req.client_ip;
        if (!is_trusted(req.peer_ip)) return true; // only proxies may vouch for a client

        auto it = req.headers.find(header);
        if (it == req.headers.end()) return true;

        // Right to left: each trusted hop vouches for the address before it.
        std::vector<std::string_view> chain = forwarded_chain(it->second, rfc7239);
        std::string client = req.peer_ip;
        for (auto hop = chain.rbegin(); hop != chain.rend(); ++hop) {
            std::array<uint8_t, 16> scratch{};
            bool is_v4 = false;
            if (!parse_ip(*hop, scratch, is_v4)) break; // "unknown", obfuscated or garbage: stop here
            client = std::string(*hop);
            if (!is_trusted(*hop)) break; // first untrusted hop is the client
        }
        req.client_ip = client;
        return true;
    };
}

} // namespace middleware
