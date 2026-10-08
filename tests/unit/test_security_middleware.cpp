#include <gtest/gtest.h>
#include <orbit/middleware/SecurityHeaders.hpp>
#include <orbit/middleware/TrustedProxies.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>

using namespace orbit::http;
using orbit::middleware::IpNetwork;

namespace {

class HeaderWriter : public ResponseWriter {
public:
    std::map<std::string, std::string> defaults;
    void set_header(const std::string& key, const std::string& value) override { defaults[key] = value; }
    void send(HttpResponse&&) override {}
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

// Runs trusted_proxies() on a request from `peer` carrying `value` in `header`.
std::string client_ip_after(const orbit::middleware::TrustedProxyOptions& opts, const std::string& peer,
                            const std::string& header, const std::string& value) {
    auto mw = orbit::middleware::trusted_proxies(opts);
    HttpRequest req;
    req.client_ip = peer;
    if (!header.empty()) req.headers[header] = value;
    auto w = std::make_shared<HeaderWriter>();
    EXPECT_TRUE(mw(req, w));
    EXPECT_EQ(req.peer_ip, peer);
    return req.client_ip;
}

} // namespace

TEST(IpNetworkTest, MatchesAddressesAndCidrs) {
    auto lo = IpNetwork::parse("127.0.0.1");
    ASSERT_TRUE(lo);
    EXPECT_TRUE(lo->contains("127.0.0.1"));
    EXPECT_FALSE(lo->contains("127.0.0.2"));

    auto ten = IpNetwork::parse("10.0.0.0/8");
    ASSERT_TRUE(ten);
    EXPECT_TRUE(ten->contains("10.255.1.2"));
    EXPECT_FALSE(ten->contains("11.0.0.1"));
    EXPECT_FALSE(ten->contains("::1"));

    auto odd = IpNetwork::parse("192.168.1.128/25");
    ASSERT_TRUE(odd);
    EXPECT_TRUE(odd->contains("192.168.1.200"));
    EXPECT_FALSE(odd->contains("192.168.1.100"));

    auto v6 = IpNetwork::parse("fd00::/8");
    ASSERT_TRUE(v6);
    EXPECT_TRUE(v6->contains("fd12:3456::1"));
    EXPECT_FALSE(v6->contains("fe80::1"));
    EXPECT_FALSE(v6->contains("10.0.0.1"));

    EXPECT_FALSE(IpNetwork::parse("10.0.0.0/33"));
    EXPECT_FALSE(IpNetwork::parse("not-an-ip"));
    EXPECT_FALSE(IpNetwork::parse("10.0.0.0/"));
    EXPECT_FALSE(ten->contains("garbage"));
}

TEST(TrustedProxiesTest, UntrustedPeerCannotSpoofItsAddress) {
    orbit::middleware::TrustedProxyOptions opts;
    opts.proxies = {"10.0.0.0/8"};
    EXPECT_EQ(client_ip_after(opts, "203.0.113.9", "X-Forwarded-For", "1.2.3.4"), "203.0.113.9");
}

TEST(TrustedProxiesTest, TrustedProxySuppliesTheClient) {
    orbit::middleware::TrustedProxyOptions opts;
    opts.proxies = {"10.0.0.0/8"};
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "X-Forwarded-For", "198.51.100.7"), "198.51.100.7");
    // No header: the proxy itself is all we know.
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "", ""), "10.0.0.5");
}

TEST(TrustedProxiesTest, ChainIsReadRightToLeftSkippingTrustedHops) {
    orbit::middleware::TrustedProxyOptions opts;
    opts.proxies = {"10.0.0.0/8", "192.168.0.1"};
    // A client-supplied fake entry on the left must not win over the real
    // client recorded by our first proxy.
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "X-Forwarded-For", "6.6.6.6, 198.51.100.7, 192.168.0.1"),
              "198.51.100.7");
    // Every hop trusted: the leftmost is the best answer.
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "X-Forwarded-For", "10.1.1.1, 192.168.0.1"), "10.1.1.1");
    // Ports and IPv6 brackets are tolerated.
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "X-Forwarded-For", "198.51.100.7:4711"), "198.51.100.7");
    // Garbage stops the walk at the last good address.
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "X-Forwarded-For", "nonsense, 192.168.0.1"), "192.168.0.1");
}

TEST(TrustedProxiesTest, Rfc7239ForwardedHeader) {
    orbit::middleware::TrustedProxyOptions opts;
    opts.proxies = {"10.0.0.0/8"};
    opts.header = "Forwarded";
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "Forwarded",
                              "for=192.0.2.60;proto=https;by=10.0.0.5, For=\"[2001:db8:cafe::17]:4711\""),
              "2001:db8:cafe::17");
    EXPECT_EQ(client_ip_after(opts, "10.0.0.5", "Forwarded", "for=unknown"), "10.0.0.5");
}

TEST(TrustedProxiesTest, MalformedConfigurationThrows) {
    orbit::middleware::TrustedProxyOptions opts;
    opts.proxies = {"10.0.0.0/8", "localhost"};
    EXPECT_THROW(orbit::middleware::trusted_proxies(opts), std::invalid_argument);
}

TEST(SecurityHeadersTest, DefaultsAreSet) {
    auto mw = orbit::middleware::security_headers();
    HttpRequest req;
    auto w = std::make_shared<HeaderWriter>();
    EXPECT_TRUE(mw(req, w));
    EXPECT_EQ(w->defaults["Strict-Transport-Security"], "max-age=31536000; includeSubDomains");
    EXPECT_EQ(w->defaults["X-Content-Type-Options"], "nosniff");
    EXPECT_EQ(w->defaults["X-Frame-Options"], "DENY");
    EXPECT_EQ(w->defaults["Referrer-Policy"], "strict-origin-when-cross-origin");
    EXPECT_EQ(w->defaults["Cross-Origin-Opener-Policy"], "same-origin");
    EXPECT_EQ(w->defaults.count("Content-Security-Policy"), 0u);
}

TEST(SecurityHeadersTest, OptionsChangeOrRemoveHeaders) {
    orbit::middleware::SecurityHeadersOptions opts;
    opts.hsts = false;
    opts.frame_options = "";
    opts.content_security_policy = "default-src 'self'; frame-ancestors 'none'";
    auto mw = orbit::middleware::security_headers(opts);
    HttpRequest req;
    auto w = std::make_shared<HeaderWriter>();
    mw(req, w);
    EXPECT_EQ(w->defaults.count("Strict-Transport-Security"), 0u);
    EXPECT_EQ(w->defaults.count("X-Frame-Options"), 0u);
    EXPECT_EQ(w->defaults["Content-Security-Policy"], "default-src 'self'; frame-ancestors 'none'");
}
