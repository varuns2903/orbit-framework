#include <gtest/gtest.h>
#include <orbit/http/WebSocketConnection.hpp>

#include <vector>

using http::websocket::detail::FrameHeader;
using http::websocket::detail::FrameStatus;
using http::websocket::detail::inspect_frame;
using http::websocket::detail::parse_frame_header;

namespace {

std::vector<char> bytes(std::initializer_list<int> values) {
    std::vector<char> out;
    for (int v : values) out.push_back(static_cast<char>(v));
    return out;
}

} // namespace

TEST(WebSocketLimitsTest, RejectsSixtyFourBitLengthWithMostSignificantBitSet) {
    // Length 0xFFFFFFFFFFFFFFF8: previously header_length + payload_length wrapped
    // around and the frame was reported as complete.
    auto buf = bytes({0x82, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 1, 2, 3, 4, 5, 6});
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::Invalid);
    EXPECT_FALSE(parse_frame_header(buf, h));
}

TEST(WebSocketLimitsTest, LargeLengthDoesNotWrapCompletenessCheck) {
    // 0x7FFFFFFFFFFFFFFF is legal but can never be complete in a small buffer.
    auto buf = bytes({0x82, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 9, 9});
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::NeedPayload);
    EXPECT_EQ(h.payload_length, 0x7FFFFFFFFFFFFFFFull);
    EXPECT_EQ(h.header_length, 14u);
}

TEST(WebSocketLimitsTest, ReportsPayloadLengthBeforePayloadArrives) {
    auto buf = bytes({0x81, 0x7E, 0x10, 0x00}); // 4096-byte text frame, no payload yet
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::NeedPayload);
    EXPECT_EQ(h.payload_length, 4096u);
}

TEST(WebSocketLimitsTest, DistinguishesIncompleteHeader) {
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(bytes({0x81}), h), FrameStatus::NeedHeader);
    EXPECT_EQ(inspect_frame(bytes({0x81, 0x7F, 0x00}), h), FrameStatus::NeedHeader);
    EXPECT_EQ(inspect_frame(bytes({0x81, 0x85, 0x01, 0x02}), h), FrameStatus::NeedHeader);
}

TEST(WebSocketLimitsTest, RejectsOversizedControlFrame) {
    auto buf = bytes({0x89, 0x7E, 0x00, 0x80}); // ping claiming 128 bytes
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::Invalid);
}

TEST(WebSocketLimitsTest, RejectsFragmentedControlFrame) {
    auto buf = bytes({0x09, 0x00}); // ping with FIN clear
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::Invalid);
}

TEST(WebSocketLimitsTest, AcceptsMaximumControlFramePayload) {
    std::vector<char> buf = bytes({0x89, 0x7D});
    buf.resize(2 + 125, 'p');
    FrameHeader h{};
    EXPECT_EQ(inspect_frame(buf, h), FrameStatus::Complete);
}
