#include <gtest/gtest.h>
#include <orbit/http/HttpParser.hpp>

using namespace http;

// --- parse_framing (RFC 9112 section 6) ---

TEST(FramingTest, NoBodyHeaders) {
    auto f = parse_framing("Host: a\r\n");
    EXPECT_TRUE(f.valid);
    EXPECT_FALSE(f.chunked);
    EXPECT_FALSE(f.has_content_length);
}

TEST(FramingTest, ContentLengthIsCaseInsensitiveAndTrimmed) {
    auto f = parse_framing("content-LENGTH:   42 \r\n");
    ASSERT_TRUE(f.valid);
    EXPECT_TRUE(f.has_content_length);
    EXPECT_EQ(f.content_length, 42u);
}

TEST(FramingTest, RejectsNonNumericContentLength) {
    EXPECT_FALSE(parse_framing("Content-Length: abc\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: -1\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: +5\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: 5abc\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: \r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: 99999999999999999999999\r\n").valid);
}

TEST(FramingTest, RejectsConflictingDuplicateContentLength) {
    EXPECT_FALSE(parse_framing("Content-Length: 1\r\nContent-Length: abc\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: 1\r\nContent-Length: 2\r\n").valid);
}

TEST(FramingTest, AcceptsIdenticalDuplicateContentLength) {
    auto f = parse_framing("Content-Length: 3\r\nContent-Length: 3\r\n");
    EXPECT_TRUE(f.valid);
    EXPECT_EQ(f.content_length, 3u);
}

TEST(FramingTest, RejectsTransferEncodingWithContentLength) {
    EXPECT_FALSE(parse_framing("Transfer-Encoding: chunked\r\nContent-Length: 5\r\n").valid);
    EXPECT_FALSE(parse_framing("Content-Length: 5\r\nTransfer-Encoding: chunked\r\n").valid);
}

TEST(FramingTest, RequiresChunkedAsFinalCoding) {
    EXPECT_TRUE(parse_framing("Transfer-Encoding: gzip, chunked\r\n").chunked);
    EXPECT_TRUE(parse_framing("Transfer-Encoding: CHUNKED\r\n").valid);
    EXPECT_FALSE(parse_framing("Transfer-Encoding: chunked, gzip\r\n").valid);
    EXPECT_FALSE(parse_framing("Transfer-Encoding: identity\r\n").valid);
}

TEST(FramingTest, RejectsWhitespaceBeforeColon) {
    EXPECT_FALSE(parse_framing("Content-Length : 5\r\n").valid);
    EXPECT_FALSE(parse_framing("Transfer-Encoding\t: chunked\r\n").valid);
}

TEST(FramingTest, RejectsObsFoldAndLinesWithoutColon) {
    EXPECT_FALSE(parse_framing("X-A: 1\r\n continued\r\n").valid);
    EXPECT_FALSE(parse_framing("NoColonHere\r\n").valid);
}

TEST(FramingTest, IgnoresFramingTextInsideOtherHeaders) {
    auto f = parse_framing("X-Note: content-length: 999\r\n");
    EXPECT_TRUE(f.valid);
    EXPECT_FALSE(f.has_content_length);
}

// --- decode_chunked (RFC 9112 section 7.1) ---

TEST(ChunkedTest, DecodesCompleteBody) {
    std::string out;
    size_t consumed = 0;
    std::string_view data = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\nNEXT";
    ASSERT_EQ(decode_chunked(data, out, consumed, 1024), ChunkedStatus::Complete);
    EXPECT_EQ(out, "hello world");
    EXPECT_EQ(data.substr(consumed), "NEXT");
}

TEST(ChunkedTest, HandlesTrailers) {
    std::string out;
    size_t consumed = 0;
    std::string_view data = "3\r\nabc\r\n0\r\nX-Trailer: 1\r\n\r\n";
    ASSERT_EQ(decode_chunked(data, out, consumed, 1024), ChunkedStatus::Complete);
    EXPECT_EQ(out, "abc");
    EXPECT_EQ(consumed, data.size());
}

TEST(ChunkedTest, ReportsIncomplete) {
    std::string out;
    size_t consumed = 0;
    EXPECT_EQ(decode_chunked("5\r\nhel", out, consumed, 1024), ChunkedStatus::Incomplete);
    EXPECT_EQ(decode_chunked("5\r\nhello\r\n", out, consumed, 1024), ChunkedStatus::Incomplete);
    EXPECT_EQ(decode_chunked("0\r\n", out, consumed, 1024), ChunkedStatus::Incomplete);
}

TEST(ChunkedTest, RejectsMalformedSizesAndMissingCrlf) {
    std::string out;
    size_t consumed = 0;
    EXPECT_EQ(decode_chunked("zz\r\n", out, consumed, 1024), ChunkedStatus::Invalid);
    EXPECT_EQ(decode_chunked("\r\n", out, consumed, 1024), ChunkedStatus::Invalid);
    EXPECT_EQ(decode_chunked("3\r\nabcXX", out, consumed, 1024), ChunkedStatus::Invalid);
    EXPECT_EQ(decode_chunked("fffffffffffffffff\r\n", out, consumed, 1024), ChunkedStatus::Invalid);
}

TEST(ChunkedTest, EnforcesMaxSize) {
    std::string out;
    size_t consumed = 0;
    EXPECT_EQ(decode_chunked("a\r\n0123456789\r\n0\r\n\r\n", out, consumed, 5), ChunkedStatus::TooLarge);
    EXPECT_EQ(decode_chunked("ffffffffffffffff\r\n", out, consumed, 5), ChunkedStatus::TooLarge);
}

// --- HttpParser uses the same framing rules ---

TEST(FramingTest, ParserRejectsAmbiguousFraming) {
    EXPECT_FALSE(HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: abc\r\n\r\nA").has_value());
    EXPECT_FALSE(HttpParser::parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n").has_value());
}

TEST(FramingTest, ParserSlicesBodyToContentLength) {
    auto req = HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: 3\r\n\r\nabcGET /next HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_EQ(req->body, "abc");
}

TEST(FramingTest, ParserGivesEmptyBodyWithoutFramingHeaders) {
    auto req = HttpParser::parse("GET / HTTP/1.1\r\nHost: a\r\n\r\nGET /next HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    EXPECT_TRUE(req->body.empty());
}
