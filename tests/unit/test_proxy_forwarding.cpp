#include <gtest/gtest.h>
#include <orbit/middleware/Proxy.hpp>

using middleware::detail::ChunkedDecoder;
using middleware::detail::ProxiedRequest;
using middleware::detail::build_upstream_request;

namespace {

ProxiedRequest sample() {
    ProxiedRequest r;
    r.method = "POST";
    r.target = "/api/items?page=2&sort=name";
    r.client_ip = "203.0.113.7";
    r.body = "hello";
    r.headers = {
        {"host", "public.example"},
        {"Content-Type", "text/plain"},
        {"connection", "keep-alive, X-Secret-Hop"},
        {"X-Secret-Hop", "drop me"},
        {"Keep-Alive", "timeout=5"},
        {"transfer-encoding", "chunked"},
        {"Proxy-Authorization", "Basic abc"},
        {"Content-Length", "999"},
        {"X-Forwarded-For", "10.0.0.1"},
        {"X-Real-IP", "10.0.0.1"},
    };
    return r;
}

size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
    return n;
}

} // namespace

TEST(ProxyForwardingTest, KeepsQueryStringAndMethod) {
    std::string out = build_upstream_request(sample(), "backend", 9000, "", false);
    EXPECT_EQ(out.rfind("POST /api/items?page=2&sort=name HTTP/1.1\r\n", 0), 0u) << out;
}

TEST(ProxyForwardingTest, SendsExactlyOneHostHeader) {
    std::string out = build_upstream_request(sample(), "backend", 9000, "", false);
    EXPECT_NE(out.find("Host: backend:9000\r\n"), std::string::npos);
    EXPECT_EQ(count(out, "\r\nHost: ") + count(out, "\r\nhost: "), 1u) << out;
    EXPECT_NE(out.find("X-Forwarded-Host: public.example\r\n"), std::string::npos) << out;
}

TEST(ProxyForwardingTest, DropsHopByHopHeadersCaseInsensitively) {
    std::string out = build_upstream_request(sample(), "backend", 9000, "", false);
    EXPECT_EQ(out.find("X-Secret-Hop"), std::string::npos) << out;
    EXPECT_EQ(out.find("Keep-Alive"), std::string::npos) << out;
    EXPECT_EQ(out.find("transfer-encoding"), std::string::npos) << out;
    EXPECT_EQ(out.find("Proxy-Authorization"), std::string::npos) << out;
    EXPECT_EQ(out.find("999"), std::string::npos) << out;
    EXPECT_NE(out.find("Content-Length: 5\r\n"), std::string::npos) << out;
    EXPECT_NE(out.find("Content-Type: text/plain\r\n"), std::string::npos) << out;
    EXPECT_EQ(out.substr(out.size() - 5), "hello");
}

TEST(ProxyForwardingTest, ReplacesClientForwardingHeadersByDefault) {
    std::string out = build_upstream_request(sample(), "backend", 9000, "", false);
    EXPECT_NE(out.find("X-Forwarded-For: 203.0.113.7\r\n"), std::string::npos) << out;
    EXPECT_NE(out.find("X-Real-IP: 203.0.113.7\r\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("10.0.0.1"), std::string::npos) << out;
}

TEST(ProxyForwardingTest, ExtendsForwardingHeadersWhenTrusted) {
    std::string out = build_upstream_request(sample(), "backend", 9000, "", true);
    EXPECT_NE(out.find("X-Forwarded-For: 10.0.0.1, 203.0.113.7\r\n"), std::string::npos) << out;
    EXPECT_EQ(count(out, "X-Forwarded-For"), 1u);
}

TEST(ProxyForwardingTest, StripsPrefixAndKeepsQuery) {
    ProxiedRequest r = sample();
    r.target = "/api/items?x=1";
    std::string out = build_upstream_request(r, "b", 1, "/api", false);
    EXPECT_EQ(out.rfind("POST /items?x=1 HTTP/1.1", 0), 0u) << out;
    r.target = "/api?x=1";
    out = build_upstream_request(r, "b", 1, "/api", false);
    EXPECT_EQ(out.rfind("POST /?x=1 HTTP/1.1", 0), 0u) << out;
}

TEST(ProxyForwardingTest, ForwardsWebSocketUpgrade) {
    ProxiedRequest r;
    r.method = "GET";
    r.target = "/ws";
    r.headers = {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Key", "k"}};
    std::string out = build_upstream_request(r, "b", 1, "", false);
    EXPECT_NE(out.find("Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n"), std::string::npos) << out;
    EXPECT_NE(out.find("Sec-WebSocket-Key: k\r\n"), std::string::npos);
}

TEST(ProxyChunkedDecoderTest, DecodesAcrossArbitrarySplits) {
    const std::string encoded = "4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nX-Trailer: y\r\n\r\n";
    for (size_t split = 1; split < encoded.size(); ++split) {
        ChunkedDecoder d;
        std::string out;
        auto r1 = d.feed(std::string_view(encoded).substr(0, split), out);
        EXPECT_NE(r1, ChunkedDecoder::Result::Error) << split;
        auto r2 = d.feed(std::string_view(encoded).substr(split), out);
        EXPECT_EQ(r2, ChunkedDecoder::Result::Done) << split;
        EXPECT_EQ(out, "Wikipedia") << split;
    }
}

TEST(ProxyChunkedDecoderTest, RejectsGarbage) {
    ChunkedDecoder d;
    std::string out;
    EXPECT_EQ(d.feed("zz\r\n", out), ChunkedDecoder::Result::Error);
    ChunkedDecoder d2;
    EXPECT_EQ(d2.feed("2\r\nabXY", out), ChunkedDecoder::Result::Error);
}
