#include <gtest/gtest.h>
#include <orbit/http/WebSocketConnection.hpp>

#include <zlib.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using http::websocket::WebSocketConnection;
namespace detail = http::websocket::detail;

namespace {

// Builds a client frame. Clients must mask; `masked = false` builds an invalid one.
std::vector<char> frame(uint8_t opcode, const std::string& payload, bool fin = true, bool masked = true,
                        uint8_t rsv = 0) {
    std::vector<char> out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0) | rsv | opcode));
    size_t len = payload.size();
    uint8_t mask_bit = masked ? 0x80 : 0;
    if (len <= 125) {
        out.push_back(static_cast<char>(mask_bit | len));
    } else {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    }
    const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
    if (masked) out.insert(out.end(), key, key + 4);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = static_cast<uint8_t>(payload[i]);
        out.push_back(static_cast<char>(masked ? (b ^ key[i % 4]) : b));
    }
    return out;
}

struct SentFrame {
    uint8_t first_byte;
    std::string payload;
    uint8_t opcode() const { return first_byte & 0x0F; }
    bool rsv1() const { return (first_byte & 0x40) != 0; }
};

// Server frames are unmasked; parse the whole byte stream back into frames.
std::vector<SentFrame> parse_server_frames(const std::vector<char>& bytes) {
    std::vector<SentFrame> frames;
    size_t i = 0;
    while (i + 2 <= bytes.size()) {
        uint8_t b0 = static_cast<uint8_t>(bytes[i]);
        uint64_t len = static_cast<uint8_t>(bytes[i + 1]) & 0x7F;
        size_t pos = i + 2;
        if (len == 126) {
            len = (static_cast<uint64_t>(static_cast<uint8_t>(bytes[pos])) << 8) | static_cast<uint8_t>(bytes[pos + 1]);
            pos += 2;
        } else if (len == 127) {
            len = 0;
            for (int k = 0; k < 8; ++k) len = (len << 8) | static_cast<uint8_t>(bytes[pos + static_cast<size_t>(k)]);
            pos += 8;
        }
        frames.push_back({b0, std::string(bytes.data() + pos, static_cast<size_t>(len))});
        i = pos + static_cast<size_t>(len);
    }
    return frames;
}

// Raw DEFLATE as permessage-deflate expects: sync flush, trailer stripped,
// and a fresh context for every message.
std::string deflate_message(const std::string& in) {
    z_stream zs{};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out(in.size() + 64, '\0');
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_SYNC_FLUSH);
    out.resize(out.size() - zs.avail_out);
    deflateEnd(&zs);
    if (out.size() >= 4) out.resize(out.size() - 4); // 00 00 ff ff
    return out;
}

std::string inflate_message(const std::string& in) {
    z_stream zs{};
    inflateInit2(&zs, -15);
    std::string data = in + std::string("\x00\x00\xff\xff", 4);
    zs.next_in = reinterpret_cast<Bytef*>(&data[0]);
    zs.avail_in = static_cast<uInt>(data.size());
    std::string out(65536, '\0');
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    inflate(&zs, Z_SYNC_FLUSH);
    out.resize(out.size() - zs.avail_out);
    inflateEnd(&zs);
    return out;
}

struct Harness {
    std::vector<char> sent; // everything the server wrote
    bool transport_closed = false;
    int close_handler_calls = 0;
    std::vector<std::string> messages;
    std::vector<char> inbox;
    std::unique_ptr<WebSocketConnection> ws;

    explicit Harness(bool deflate = false) {
        WebSocketConnection::Transport t{
            [this](const std::vector<char>& d) { sent.insert(sent.end(), d.begin(), d.end()); },
            [this]() { transport_closed = true; }};
        ws = std::make_unique<WebSocketConnection>(std::move(t), deflate);
        ws->on_message([this](const std::string& m) { messages.push_back(m); });
        ws->on_close([this]() { ++close_handler_calls; });
    }

    void feed(const std::vector<char>& bytes) {
        inbox.insert(inbox.end(), bytes.begin(), bytes.end());
        ws->process_raw_data(inbox);
    }

    std::vector<SentFrame> frames() const { return parse_server_frames(sent); }

    // Status code of the close frame the server sent, -1 if none, 0 if it had no code.
    int close_code() const {
        for (const auto& f : frames()) {
            if (f.opcode() == 0x8) {
                if (f.payload.size() < 2) return 0;
                return (static_cast<uint8_t>(f.payload[0]) << 8) | static_cast<uint8_t>(f.payload[1]);
            }
        }
        return -1;
    }
};

} // namespace

TEST(WebSocketProtocolTest, FragmentedMessageIsReassembledAroundControlFrames) {
    Harness h;
    h.feed(frame(0x1, "Hel", false));
    h.feed(frame(0x9, "ping!"));           // control frames may interleave
    h.feed(frame(0x0, "lo ", false));
    EXPECT_TRUE(h.messages.empty());
    h.feed(frame(0x0, "world", true));
    ASSERT_EQ(h.messages.size(), 1u);
    EXPECT_EQ(h.messages[0], "Hello world");
}

TEST(WebSocketProtocolTest, PongEchoesThePingPayload) {
    Harness h;
    h.feed(frame(0x9, "payload-123"));
    auto f = h.frames();
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].opcode(), 0xA);
    EXPECT_EQ(f[0].payload, "payload-123");
}

TEST(WebSocketProtocolTest, UnmaskedClientFrameFailsWith1002) {
    Harness h;
    h.feed(frame(0x1, "hi", true, false));
    EXPECT_EQ(h.close_code(), 1002);
    EXPECT_TRUE(h.transport_closed);
    EXPECT_TRUE(h.messages.empty());
}

TEST(WebSocketProtocolTest, InvalidUtf8TextFailsWith1007) {
    Harness h;
    h.feed(frame(0x1, "\xC0\xAF")); // overlong '/'
    EXPECT_EQ(h.close_code(), 1007);
    EXPECT_TRUE(h.messages.empty());
}

TEST(WebSocketProtocolTest, Utf8SplitAcrossFragmentsIsAccepted) {
    Harness h;
    std::string text = "\xCE\xBA\xE1\xBD\xB9\xCF\x83\xCE\xBC\xCE\xB5"; // "κόσμε"
    h.feed(frame(0x1, text.substr(0, 3), false)); // cuts a multi-byte sequence
    h.feed(frame(0x0, text.substr(3), true));
    ASSERT_EQ(h.messages.size(), 1u);
    EXPECT_EQ(h.messages[0], text);
}

TEST(WebSocketProtocolTest, BinaryMessagesAreNotUtf8Checked) {
    Harness h;
    h.feed(frame(0x2, std::string("\xFF\xFE\x00\x01", 4)));
    ASSERT_EQ(h.messages.size(), 1u);
    EXPECT_EQ(h.close_code(), -1);
}

TEST(WebSocketProtocolTest, Utf8Validator) {
    EXPECT_TRUE(detail::is_valid_utf8(""));
    EXPECT_TRUE(detail::is_valid_utf8("plain ascii"));
    EXPECT_TRUE(detail::is_valid_utf8("\xF0\x9F\x98\x80")); // U+1F600
    EXPECT_TRUE(detail::is_valid_utf8("\xF4\x8F\xBF\xBF")); // U+10FFFF
    EXPECT_FALSE(detail::is_valid_utf8("\xF4\x90\x80\x80")); // > U+10FFFF
    EXPECT_FALSE(detail::is_valid_utf8("\xED\xA0\x80"));     // surrogate
    EXPECT_FALSE(detail::is_valid_utf8("\xE0\x80\xAF"));     // overlong
    EXPECT_FALSE(detail::is_valid_utf8("\xF0\x80\x80\xAF")); // overlong
    EXPECT_FALSE(detail::is_valid_utf8("\xC3"));             // truncated
    EXPECT_FALSE(detail::is_valid_utf8("\x80"));             // stray continuation
}

TEST(WebSocketProtocolTest, ReservedBitsWithoutAnExtensionFail) {
    Harness rsv1;
    rsv1.feed(frame(0x1, "hi", true, true, 0x40)); // no permessage-deflate negotiated
    EXPECT_EQ(rsv1.close_code(), 1002);

    Harness rsv2(true);
    rsv2.feed(frame(0x1, "hi", true, true, 0x20));
    EXPECT_EQ(rsv2.close_code(), 1002);

    Harness control(true);
    control.feed(frame(0x9, "x", true, true, 0x40)); // RSV1 is never valid on control frames
    EXPECT_EQ(control.close_code(), 1002);
}

TEST(WebSocketProtocolTest, ReservedOpcodesFail) {
    for (uint8_t opcode : std::initializer_list<uint8_t>{0x3, 0x7, 0xB, 0xF}) {
        Harness h;
        h.feed(frame(opcode, ""));
        EXPECT_EQ(h.close_code(), 1002) << int(opcode);
    }
}

TEST(WebSocketProtocolTest, FragmentSequenceErrorsFail) {
    Harness orphan;
    orphan.feed(frame(0x0, "nothing to continue"));
    EXPECT_EQ(orphan.close_code(), 1002);

    Harness interleaved;
    interleaved.feed(frame(0x1, "first", false));
    interleaved.feed(frame(0x1, "second")); // new message before the first finished
    EXPECT_EQ(interleaved.close_code(), 1002);
    EXPECT_TRUE(interleaved.messages.empty());
}

TEST(WebSocketProtocolTest, FragmentedMessageIsSizeLimitedAsAWhole) {
    Harness h;
    h.ws->set_max_message_size(10);
    h.feed(frame(0x1, "123456", false));
    h.feed(frame(0x0, "789012", true));
    EXPECT_EQ(h.close_code(), 1009);
    EXPECT_TRUE(h.messages.empty());
}

TEST(WebSocketProtocolTest, CloseEchoesTheStatusCode) {
    Harness h;
    h.feed(frame(0x8, std::string("\x03\xE8", 2) + "bye"));
    EXPECT_EQ(h.close_code(), 1000);
    EXPECT_TRUE(h.transport_closed);
    EXPECT_EQ(h.close_handler_calls, 1);

    Harness empty;
    empty.feed(frame(0x8, ""));
    EXPECT_EQ(empty.close_code(), 0); // a close without a code is answered without one
    EXPECT_TRUE(empty.transport_closed);
}

TEST(WebSocketProtocolTest, InvalidCloseFramesFail) {
    struct Case {
        std::string payload;
        int expected;
    };
    for (const Case& c : {Case{std::string("\x03", 1), 1002},                // 1-byte payload
                          Case{std::string("\x03\xED", 2), 1002},             // 1005 must not be sent
                          Case{std::string("\x03\xEC", 2), 1002},             // 1004 reserved
                          Case{std::string("\x03\xE7", 2), 1002},             // 999
                          Case{std::string("\x03\xE8\xC0\xAF", 4), 1007}}) {  // invalid UTF-8 reason
        Harness h;
        h.feed(frame(0x8, c.payload));
        EXPECT_EQ(h.close_code(), c.expected);
    }

    Harness app_code;
    app_code.feed(frame(0x8, std::string("\x0F\xA0", 2))); // 4000: application-defined
    EXPECT_EQ(app_code.close_code(), 4000);
}

TEST(WebSocketProtocolTest, NothingIsProcessedAfterClose) {
    Harness h;
    auto bytes = frame(0x8, std::string("\x03\xE8", 2));
    auto after = frame(0x1, "late");
    bytes.insert(bytes.end(), after.begin(), after.end());
    h.feed(bytes);
    EXPECT_TRUE(h.messages.empty());
    h.ws->send("ignored");
    EXPECT_EQ(h.frames().size(), 1u); // only the close reply
}

TEST(WebSocketProtocolTest, CompressedFragmentedMessageIsInflated) {
    Harness h(true);
    std::string text(500, 'a');
    std::string compressed = deflate_message(text);
    size_t half = compressed.size() / 2;
    h.feed(frame(0x1, compressed.substr(0, half), false, true, 0x40)); // RSV1 only on the first frame
    h.feed(frame(0x0, compressed.substr(half), true));
    ASSERT_EQ(h.messages.size(), 1u);
    EXPECT_EQ(h.messages[0], text);

    // A second message compressed with a fresh context also decodes.
    h.feed(frame(0x1, deflate_message("again"), true, true, 0x40));
    ASSERT_EQ(h.messages.size(), 2u);
    EXPECT_EQ(h.messages[1], "again");
}

TEST(WebSocketProtocolTest, ServerCompressesEachMessageIndependently) {
    // server_no_context_takeover: the second identical message must not refer
    // back to the first, so both frames are identical and decode on their own.
    Harness h(true);
    h.ws->send("hello hello hello hello");
    h.ws->send("hello hello hello hello");
    auto f = h.frames();
    ASSERT_EQ(f.size(), 2u);
    EXPECT_TRUE(f[0].rsv1());
    EXPECT_EQ(f[0].payload, f[1].payload);
    EXPECT_EQ(inflate_message(f[1].payload), "hello hello hello hello");
}

TEST(WebSocketProtocolTest, ConcurrentSendsProduceWholeFrames) {
    Harness h(true);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&h, t] {
            for (int i = 0; i < 200; ++i) {
                h.ws->send("thread " + std::to_string(t) + " message " + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    auto f = h.frames();
    ASSERT_EQ(f.size(), 1600u);
    for (const auto& frame_out : f) {
        std::string text = inflate_message(frame_out.payload);
        ASSERT_EQ(text.rfind("thread ", 0), 0u) << text;
    }
}

TEST(WebSocketProtocolTest, SendBinaryUsesOpcode2) {
    Harness h;
    h.ws->send_binary(std::string("\x00\xFF", 2));
    auto f = h.frames();
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].opcode(), 0x2);
    EXPECT_EQ(f[0].payload, std::string("\x00\xFF", 2));
}
