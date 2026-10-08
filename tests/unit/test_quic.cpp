#include <gtest/gtest.h>

#ifdef ORBIT_ENABLE_HTTP3

#include <orbit/server/QuicConnection.hpp>
#include <orbit/server/QuicConnectionManager.hpp>
#include <orbit/server/QuicHttp3Session.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using orbit::server::quic::detail::ErrorAction;

namespace {

std::vector<uint8_t> from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// The client Initial from RFC 9001 Appendix A.2, header only: long header,
// Initial, packet number length 4, version 1, an 8-byte DCID, no SCID, no
// token, Length 1182, packet number 2. The protected payload is not needed
// to decide acceptance, so zeros stand in for it (1200 bytes in total).
std::vector<uint8_t> rfc9001_client_initial() {
    std::vector<uint8_t> pkt = from_hex("c300000001088394c8f03e5157080000449e00000002");
    pkt.resize(1200, 0);
    return pkt;
}

// A long-header packet of @p type (0 Initial, 1 0-RTT, 2 Handshake) with a
// @p dcidlen-byte DCID, whose Length field makes it @p total bytes long.
std::vector<uint8_t> long_packet(uint8_t type, size_t dcidlen, size_t total) {
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0xc0 | (type << 4))); // packet number length 1
    for (uint8_t b : {0x00, 0x00, 0x00, 0x01}) pkt.push_back(b);
    pkt.push_back(static_cast<uint8_t>(dcidlen));
    for (size_t i = 0; i < dcidlen; ++i) pkt.push_back(static_cast<uint8_t>(0xa0 + i));
    pkt.push_back(0); // no SCID
    if (type == 0) pkt.push_back(0); // Initial: empty token
    size_t length = total - pkt.size() - 2; // what follows the 2-byte Length field
    pkt.push_back(static_cast<uint8_t>(0x40 | (length >> 8)));
    pkt.push_back(static_cast<uint8_t>(length & 0xff));
    pkt.resize(total, 0);
    return pkt;
}

ngtcp2_cid cid(const std::string& bytes) {
    ngtcp2_cid c;
    ngtcp2_cid_init(&c, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    return c;
}

std::string nv_name(const nghttp3_nv& nv) { return std::string(reinterpret_cast<const char*>(nv.name), nv.namelen); }
std::string nv_value(const nghttp3_nv& nv) { return std::string(reinterpret_cast<const char*>(nv.value), nv.valuelen); }

} // namespace

// ---------------- Errors ----------------

TEST(QuicErrorActionTest, ReadErrors) {
    using orbit::server::quic::detail::on_read_error;
    // The peer sent CONNECTION_CLOSE: draining period, nothing more is sent.
    EXPECT_EQ(on_read_error(NGTCP2_ERR_DRAINING), ErrorAction::Drain);
    // ngtcp2 asks for the state to be dropped silently.
    EXPECT_EQ(on_read_error(NGTCP2_ERR_DROP_CONN), ErrorAction::Drop);
    // Protocol and crypto errors are reported to the peer.
    EXPECT_EQ(on_read_error(NGTCP2_ERR_PROTO), ErrorAction::Close);
    EXPECT_EQ(on_read_error(NGTCP2_ERR_CRYPTO), ErrorAction::Close);
    EXPECT_EQ(on_read_error(NGTCP2_ERR_FLOW_CONTROL), ErrorAction::Close);
    // An idle timeout is not something a received packet can cause.
    EXPECT_EQ(on_read_error(NGTCP2_ERR_IDLE_CLOSE), ErrorAction::Close);
}

TEST(QuicErrorActionTest, ExpiryErrors) {
    using orbit::server::quic::detail::on_expiry_error;
    // RFC 9000 section 10.1: an idle timeout closes silently.
    EXPECT_EQ(on_expiry_error(NGTCP2_ERR_IDLE_CLOSE), ErrorAction::Drop);
    // Loss detection gave up, for example: tell the peer.
    EXPECT_EQ(on_expiry_error(NGTCP2_ERR_HANDSHAKE_TIMEOUT), ErrorAction::Close);
    EXPECT_EQ(on_expiry_error(NGTCP2_ERR_INTERNAL), ErrorAction::Close);
}

// ---------------- First packets ----------------

TEST(QuicFirstPacketTest, Rfc9001ClientInitialIsAccepted) {
    auto pkt = rfc9001_client_initial();
    EXPECT_TRUE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, InitialOfExactly1200BytesIsAccepted) {
    auto pkt = long_packet(0, 8, 1200);
    EXPECT_TRUE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, InitialUnder1200BytesIsDropped) {
    // RFC 9000 section 14.1: smaller client Initials must be discarded.
    auto pkt = long_packet(0, 8, 1199);
    EXPECT_FALSE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, InitialWithShortDcidIsDropped) {
    // RFC 9000 section 7.2: the client's first DCID is at least 8 bytes.
    auto pkt = long_packet(0, 7, 1200);
    EXPECT_FALSE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, HandshakePacketCannotOpenAConnection) {
    auto pkt = long_packet(2, 8, 1200);
    EXPECT_FALSE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, ShortHeaderPacketCannotOpenAConnection) {
    std::vector<uint8_t> pkt(1200, 0);
    pkt[0] = 0x40; // short header, fixed bit set
    EXPECT_FALSE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), pkt.size()));
}

TEST(QuicFirstPacketTest, TruncatedAndEmptyInputIsDropped) {
    auto pkt = rfc9001_client_initial();
    for (size_t len : {0u, 1u, 5u, 6u, 14u, 20u}) {
        EXPECT_FALSE(orbit::server::quic::detail::acceptable_first_packet(pkt.data(), len)) << "length " << len;
    }
}

// ---------------- Connection IDs ----------------

TEST(QuicConnectionIdTest, EqualityComparesLengthAndBytes) {
    orbit::server::QuicConnectionIdEqual eq;
    EXPECT_TRUE(eq(cid("abcdefgh"), cid("abcdefgh")));
    EXPECT_FALSE(eq(cid("abcdefgh"), cid("abcdefgi")));
    EXPECT_FALSE(eq(cid("abcdefgh"), cid("abcdefg")));
    EXPECT_TRUE(eq(cid(""), cid("")));
}

TEST(QuicConnectionIdTest, HashIsStableAndUsesEveryByte) {
    orbit::server::QuicConnectionIdHash hash;
    EXPECT_EQ(hash(cid("abcdefgh")), hash(cid("abcdefgh")));
    // A prefix of the same bytes is a different ID and should land elsewhere.
    EXPECT_NE(hash(cid("abcdefgh")), hash(cid("abcdefg")));
    EXPECT_NE(hash(cid("abcdefgh")), hash(cid("abcdefgi")));
}

// ---------------- Sent chunks ----------------

TEST(QuicSentChunksTest, HandsOutInOrderAndIgnoresEmptyChunks) {
    orbit::server::quic::detail::SentChunks chunks;
    chunks.push("one");
    chunks.push("");
    chunks.push("two");
    EXPECT_EQ(chunks.held(), 2u);

    nghttp3_vec vec[4];
    ASSERT_EQ(chunks.hand_out(vec, 4), 2u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(vec[0].base), vec[0].len), "one");
    EXPECT_EQ(std::string(reinterpret_cast<char*>(vec[1].base), vec[1].len), "two");
    EXPECT_FALSE(chunks.has_unsent());
    EXPECT_EQ(chunks.hand_out(vec, 4), 0u);
}

TEST(QuicSentChunksTest, HandOutRespectsTheVectorCount) {
    orbit::server::quic::detail::SentChunks chunks;
    for (const char* c : {"a", "b", "c"}) chunks.push(c);
    nghttp3_vec vec[2];
    EXPECT_EQ(chunks.hand_out(vec, 0), 0u);
    EXPECT_EQ(chunks.hand_out(vec, 2), 2u);
    EXPECT_TRUE(chunks.has_unsent());
    EXPECT_EQ(chunks.hand_out(vec, 2), 1u);
    EXPECT_EQ(std::string(reinterpret_cast<char*>(vec[0].base), vec[0].len), "c");
}

TEST(QuicSentChunksTest, ChunksStayUntilFullyAcknowledged) {
    orbit::server::quic::detail::SentChunks chunks;
    chunks.push("hello"); // 5
    chunks.push("big world"); // 9
    nghttp3_vec vec[2];
    ASSERT_EQ(chunks.hand_out(vec, 2), 2u);
    const uint8_t* second = vec[1].base;

    chunks.ack(3); // part of "hello"
    EXPECT_EQ(chunks.held(), 2u);
    chunks.ack(4); // the rest of "hello" and 2 bytes of "big world"
    EXPECT_EQ(chunks.held(), 1u);
    // The remaining chunk was not moved: nghttp3 may still read it.
    EXPECT_EQ(std::memcmp(second, "big world", 9), 0);

    chunks.push("more"); // appending must not move held chunks either
    EXPECT_EQ(std::memcmp(second, "big world", 9), 0);

    chunks.ack(7);
    EXPECT_EQ(chunks.held(), 1u); // only "more" is left, not handed out yet
    EXPECT_TRUE(chunks.has_unsent());
}

TEST(QuicSentChunksTest, AcksNeverFreeChunksThatWereNotHandedOut) {
    orbit::server::quic::detail::SentChunks chunks;
    chunks.push("abc");
    chunks.ack(100); // more than was ever handed out
    EXPECT_EQ(chunks.held(), 1u);
    nghttp3_vec vec[1];
    EXPECT_EQ(chunks.hand_out(vec, 1), 1u);
}

// ---------------- Response headers ----------------

TEST(Http3ResponseHeadersTest, StatusFirstAndNamesLowercased) {
    orbit::http::HttpResponse res;
    res.status_code = orbit::http::HttpStatus::NotFound;
    res.headers["Content-Type"] = "text/plain";
    res.headers["X-Custom-Header"] = "MixedCase Value";

    auto block = orbit::server::quic::detail::build_response_headers(res);
    ASSERT_EQ(block.nvs.size(), 3u);
    EXPECT_EQ(nv_name(block.nvs[0]), ":status");
    EXPECT_EQ(nv_value(block.nvs[0]), "404");
    std::vector<std::pair<std::string, std::string>> rest;
    for (size_t i = 1; i < block.nvs.size(); ++i) rest.emplace_back(nv_name(block.nvs[i]), nv_value(block.nvs[i]));
    EXPECT_NE(std::find(rest.begin(), rest.end(), std::make_pair(std::string("content-type"), std::string("text/plain"))), rest.end());
    EXPECT_NE(std::find(rest.begin(), rest.end(), std::make_pair(std::string("x-custom-header"), std::string("MixedCase Value"))), rest.end());
}

TEST(Http3ResponseHeadersTest, ConnectionSpecificFieldsAreDropped) {
    orbit::http::HttpResponse res;
    res.headers["Connection"] = "keep-alive";
    res.headers["Transfer-Encoding"] = "chunked";
    res.headers["Keep-Alive"] = "timeout=5";
    res.headers["Upgrade"] = "websocket";
    res.headers["Proxy-Connection"] = "close";
    res.headers["Vary"] = "Accept";

    auto block = orbit::server::quic::detail::build_response_headers(res);
    ASSERT_EQ(block.nvs.size(), 2u);
    EXPECT_EQ(nv_name(block.nvs[1]), "vary");
}

TEST(Http3ResponseHeadersTest, EntriesPointIntoLiveStorage) {
    // Regression: the storage once reallocated on the last push, leaving the
    // short-string entries pointing at freed memory.
    for (size_t count : {0u, 1u, 2u, 7u, 33u}) {
        orbit::http::HttpResponse res;
        for (size_t i = 0; i < count; ++i) res.headers["H" + std::to_string(i)] = "v" + std::to_string(i);
        auto block = orbit::server::quic::detail::build_response_headers(res);
        ASSERT_EQ(block.nvs.size(), count + 1);
        EXPECT_EQ(nv_name(block.nvs[0]), ":status");
        EXPECT_EQ(nv_value(block.nvs[0]), "200");
        for (size_t i = 1; i < block.nvs.size(); ++i) {
            std::string name = nv_name(block.nvs[i]);
            ASSERT_EQ(name[0], 'h') << "count " << count;
            EXPECT_EQ(nv_value(block.nvs[i]), "v" + name.substr(1));
        }
    }
}

#endif // ORBIT_ENABLE_HTTP3
