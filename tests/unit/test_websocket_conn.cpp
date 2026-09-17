#include <gtest/gtest.h>
#include <orbit/http/WebSocketConnection.hpp>

#include <cstring>
#include <string>
#include <vector>

using http::websocket::detail::FrameHeader;
using http::websocket::detail::parse_frame_header;
using http::websocket::detail::unmask_payload;

namespace {

std::vector<char> bytes(std::initializer_list<int> vals) {
    std::vector<char> out;
    out.reserve(vals.size());
    for (int v : vals) out.push_back(static_cast<char>(v));
    return out;
}

void append(std::vector<char>& buf, const std::string& s) {
    buf.insert(buf.end(), s.begin(), s.end());
}

} // namespace

// --- RFC 6455 section 5.7 worked examples ---

TEST(WebSocketFrameTest, ParsesUnmaskedTextFrameFromRfcExample) {
    // 0x81 0x05 0x48 0x65 0x6c 0x6c 0x6f  -> unmasked "Hello"
    auto buf = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f});

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_TRUE(h.fin);
    EXPECT_EQ(h.opcode, 0x1);
    EXPECT_FALSE(h.masked);
    EXPECT_EQ(h.payload_length, 5u);
    EXPECT_EQ(h.header_length, 2u);

    std::string payload(buf.data() + h.header_length, h.payload_length);
    EXPECT_EQ(payload, "Hello");
}

TEST(WebSocketFrameTest, ParsesAndUnmasksMaskedTextFrameFromRfcExample) {
    // 0x81 0x85 0x37 0xfa 0x21 0x3d 0x7f 0x9f 0x4d 0x51 0x58 -> masked "Hello"
    auto buf = bytes({0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                      0x7f, 0x9f, 0x4d, 0x51, 0x58});

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_TRUE(h.fin);
    EXPECT_EQ(h.opcode, 0x1);
    EXPECT_TRUE(h.masked);
    EXPECT_EQ(h.payload_length, 5u);
    EXPECT_EQ(h.header_length, 6u); // 2 header + 4 mask key
    EXPECT_EQ(h.mask_key[0], 0x37);
    EXPECT_EQ(h.mask_key[1], 0xfa);
    EXPECT_EQ(h.mask_key[2], 0x21);
    EXPECT_EQ(h.mask_key[3], 0x3d);

    std::string payload(buf.data() + h.header_length, h.payload_length);
    unmask_payload(payload, h.mask_key);
    EXPECT_EQ(payload, "Hello");
}

TEST(WebSocketFrameTest, UnmaskingIsItsOwnInverse) {
    std::string payload = "the quick brown fox";
    const uint8_t key[4] = {0xde, 0xad, 0xbe, 0xef};
    const std::string original = payload;

    unmask_payload(payload, key);
    EXPECT_NE(payload, original);
    unmask_payload(payload, key);
    EXPECT_EQ(payload, original);
}

TEST(WebSocketFrameTest, UnmaskingHandlesEmptyPayload) {
    std::string payload;
    const uint8_t key[4] = {0x01, 0x02, 0x03, 0x04};
    unmask_payload(payload, key);
    EXPECT_TRUE(payload.empty());
}

// --- extended payload lengths ---

TEST(WebSocketFrameTest, ParsesSixteenBitExtendedLength) {
    // len == 126 means the next 2 bytes are a big-endian 16-bit length.
    auto buf = bytes({0x81, 126, 0x01, 0x00}); // 256 bytes
    append(buf, std::string(256, 'a'));

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_EQ(h.payload_length, 256u);
    EXPECT_EQ(h.header_length, 4u);
}

TEST(WebSocketFrameTest, ParsesSixtyFourBitExtendedLength) {
    // len == 127 means the next 8 bytes are a big-endian 64-bit length.
    auto buf = bytes({0x82, 127,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}); // 65536
    append(buf, std::string(65536, 'b'));

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_EQ(h.opcode, 0x2); // binary
    EXPECT_EQ(h.payload_length, 65536u);
    EXPECT_EQ(h.header_length, 10u);
}

TEST(WebSocketFrameTest, ParsesMaskedExtendedLengthHeaderLength) {
    auto buf = bytes({0x81, static_cast<int>(0x80 | 126), 0x00, 0x10,
                      0xaa, 0xbb, 0xcc, 0xdd}); // 16-byte payload, masked
    append(buf, std::string(16, 'x'));

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_TRUE(h.masked);
    EXPECT_EQ(h.payload_length, 16u);
    EXPECT_EQ(h.header_length, 8u); // 2 + 2 extended + 4 mask
    EXPECT_EQ(h.mask_key[0], 0xaa);
    EXPECT_EQ(h.mask_key[3], 0xdd);
}

// --- incomplete input must not be treated as a frame ---

TEST(WebSocketFrameTest, RejectsEmptyBuffer) {
    std::vector<char> buf;
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, RejectsSingleByteBuffer) {
    auto buf = bytes({0x81});
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, RejectsTruncatedSixteenBitLength) {
    auto buf = bytes({0x81, 126, 0x01}); // needs 2 length bytes, has 1
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, RejectsTruncatedSixtyFourBitLength) {
    auto buf = bytes({0x81, 127, 0x00, 0x00, 0x00}); // needs 8, has 3
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, RejectsTruncatedMaskKey) {
    auto buf = bytes({0x81, static_cast<int>(0x80 | 0x05), 0x37, 0xfa}); // needs 4 mask bytes
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, RejectsFrameWhosePayloadHasNotFullyArrived) {
    // Header claims 5 bytes of payload but only 3 are present.
    auto buf = bytes({0x81, 0x05, 0x48, 0x65, 0x6c});
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketFrameTest, AcceptsFrameOnceTheFinalPayloadByteArrives) {
    auto buf = bytes({0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c});
    FrameHeader h{};
    EXPECT_FALSE(parse_frame_header(buf, h));

    buf.push_back('o');
    ASSERT_TRUE(parse_frame_header(buf, h));
    EXPECT_EQ(h.payload_length, 5u);
}

// --- control frames and flags ---

TEST(WebSocketFrameTest, ParsesCloseFrame) {
    auto buf = bytes({0x88, 0x00}); // FIN + opcode 0x8, empty payload
    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_TRUE(h.fin);
    EXPECT_EQ(h.opcode, 0x8);
    EXPECT_EQ(h.payload_length, 0u);
}

TEST(WebSocketFrameTest, ParsesPingAndPongOpcodes) {
    FrameHeader h{};

    auto ping = bytes({0x89, 0x00});
    ASSERT_TRUE(parse_frame_header(ping, h));
    EXPECT_EQ(h.opcode, 0x9);

    auto pong = bytes({0x8a, 0x00});
    ASSERT_TRUE(parse_frame_header(pong, h));
    EXPECT_EQ(h.opcode, 0xa);
}

TEST(WebSocketFrameTest, ParsesContinuationFrameWithFinClear) {
    auto buf = bytes({0x00, 0x02, 'h', 'i'}); // FIN clear, opcode 0x0
    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_FALSE(h.fin);
    EXPECT_EQ(h.opcode, 0x0);
    EXPECT_EQ(h.payload_length, 2u);
}

TEST(WebSocketFrameTest, IgnoresRsv1WhenDecodingOpcode) {
    // 0xC1 = FIN | RSV1 | opcode 0x1 (permessage-deflate sets RSV1).
    auto buf = bytes({0xc1, 0x01, 0x41});
    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_TRUE(h.fin);
    EXPECT_EQ(h.opcode, 0x1);
    EXPECT_EQ(h.payload_length, 1u);
}

TEST(WebSocketFrameTest, ParsesZeroLengthTextFrame) {
    auto buf = bytes({0x81, 0x00});
    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_EQ(h.payload_length, 0u);
    EXPECT_EQ(h.header_length, 2u);
}

TEST(WebSocketFrameTest, ParsesMaximumSevenBitLength) {
    // 125 is the largest length expressible without an extension field.
    auto buf = bytes({0x81, 125});
    append(buf, std::string(125, 'z'));

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));

    EXPECT_EQ(h.payload_length, 125u);
    EXPECT_EQ(h.header_length, 2u);
}

TEST(WebSocketFrameTest, DoesNotConsumeBytesBeyondTheFrame) {
    // Two frames back to back: header_length + payload_length must describe
    // only the first, so the caller can parse the second afterwards.
    auto buf = bytes({0x81, 0x02, 'h', 'i', 0x81, 0x01, '!'});

    FrameHeader h{};
    ASSERT_TRUE(parse_frame_header(buf, h));
    EXPECT_EQ(h.header_length + h.payload_length, 4u);

    std::vector<char> rest(buf.begin() + static_cast<long>(h.header_length + h.payload_length), buf.end());
    FrameHeader h2{};
    ASSERT_TRUE(parse_frame_header(rest, h2));
    EXPECT_EQ(h2.payload_length, 1u);
    EXPECT_EQ(rest[h2.header_length], '!');
}
