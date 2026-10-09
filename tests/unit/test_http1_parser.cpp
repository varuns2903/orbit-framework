#include <gtest/gtest.h>
#include <orbit/http/Http1Parser.hpp>

#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using orbit::http::Http1Parser;
using orbit::http::HttpMethod;
using Event = Http1Parser::Event;

namespace {

// A completed request, copied out. (Copying an HttpRequest is not enough: its
// header views point into the original's storage, which next() clears.)
struct Seen {
    HttpMethod method{};
    std::string uri, target, http_version;
    std::map<std::string, std::string> headers; // lower-cased names
    std::unordered_map<std::string, std::string> query, cookies;

    const std::string& header(std::string name) const {
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return headers.at(name);
    }
};

Seen seen(const orbit::http::HttpRequest& r) {
    Seen s{r.method, r.uri, r.target, r.http_version, {}, r.query, r.cookies};
    for (const auto& [k, v] : r.headers) {
        std::string key(k);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        s.headers[key] = std::string(v);
    }
    return s;
}

// What feeding a whole input produced, at one piece size.
struct ParseRun {
    std::vector<Event> events;       // HeadersComplete / MessageComplete / Error, in order
    std::vector<Seen> requests;      // each completed request
    std::vector<std::string> bodies; // their bodies
    int error_status = 0;
    std::string error_reason;
    size_t leftover = 0;             // bytes not consumed at the end
};

// Feeds `input` in pieces of `piece` bytes, resuming after every pause and
// calling next() after each message, as a connection would.
ParseRun parse(const std::string& input, size_t piece, Http1Parser::Limits limits = {}) {
    Http1Parser p(limits);
    ParseRun run;
    size_t pos = 0;
    // Every pause is reported once per message, so the number of events is
    // bounded by the input; past that the parser is not making progress.
    // Stop rather than loop (and grow `events`) forever.
    const size_t max_events = 4 * input.size() + 16;
    while (pos < input.size()) {
        size_t end = std::min(input.size(), pos + piece);
        std::string_view chunk(input.data() + pos, end - pos);
        while (!chunk.empty()) {
            size_t used = 0;
            Event e = p.feed(chunk, used);
            chunk.remove_prefix(used);
            pos += used;
            if (e == Event::NeedMore) break;
            run.events.push_back(e);
            if (run.events.size() > max_events) {
                ADD_FAILURE() << "parser made no progress at byte " << pos;
                return run;
            }
            if (e == Event::Error) {
                run.error_status = p.error_status();
                run.error_reason = p.error_reason();
                run.leftover = input.size() - pos;
                return run;
            }
            if (e == Event::MessageComplete) {
                run.bodies.emplace_back(p.request().body);
                run.requests.push_back(seen(p.request()));
                p.next();
            }
        }
        if (chunk.empty()) pos = end;
    }
    return run;
}

// The same input at several piece sizes; every split must give one result.
std::vector<ParseRun> parse_all_ways(const std::string& input, Http1Parser::Limits limits = {}) {
    std::vector<ParseRun> runs;
    for (size_t piece : {size_t{1}, size_t{3}, size_t{7}, input.size()}) runs.push_back(parse(input, piece, limits));
    return runs;
}

const char* name(Event e) {
    switch (e) {
        case Event::NeedMore: return "NeedMore";
        case Event::HeadersComplete: return "HeadersComplete";
        case Event::MessageComplete: return "MessageComplete";
        case Event::Error: return "Error";
    }
    return "?";
}

std::string events(const ParseRun& r) {
    std::string s;
    for (Event e : r.events) s += std::string(name(e)) + " ";
    return s;
}

} // namespace

// --- Requests come out as the old parser produced them ---

TEST(Http1ParserTest, ParsesRequestLineHeadersQueryAndCookies) {
    const std::string req =
        "GET /a%20b/c?x=1&y=two%20words HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "X-Trailing-Space: value  \t\r\n"
        "Cookie: session=abc; theme=dark\r\n"
        "\r\n";
    for (const ParseRun& r : parse_all_ways(req)) {
        ASSERT_EQ(r.requests.size(), 1u) << events(r) << r.error_reason;
        const auto& q = r.requests[0];
        EXPECT_EQ(q.method, HttpMethod::GET);
        EXPECT_EQ(q.target, "/a%20b/c?x=1&y=two%20words");
        EXPECT_EQ(q.uri, "/a b/c");
        EXPECT_EQ(q.query.at("x"), "1");
        EXPECT_EQ(q.query.at("y"), "two words");
        EXPECT_EQ(q.http_version, "HTTP/1.1");
        EXPECT_EQ(q.header("Host"), "example.com");
        EXPECT_EQ(q.header("X-Trailing-Space"), "value");
        EXPECT_EQ(q.cookies.at("session"), "abc");
        EXPECT_EQ(q.cookies.at("theme"), "dark");
        EXPECT_TRUE(r.bodies[0].empty());
    }
}

TEST(Http1ParserTest, EveryOrbitMethodAndOthersAsUnknown) {
    const std::pair<const char*, HttpMethod> methods[] = {
        {"GET", HttpMethod::GET}, {"POST", HttpMethod::POST}, {"PUT", HttpMethod::PUT},
        {"PATCH", HttpMethod::PATCH}, {"DELETE", HttpMethod::DELETE}, {"OPTIONS", HttpMethod::OPTIONS},
        {"HEAD", HttpMethod::HEAD}, {"PROPFIND", HttpMethod::UNKNOWN},
    };
    for (const auto& [token, method] : methods) {
        ParseRun r = parse(std::string(token) + " / HTTP/1.1\r\nHost: h\r\n\r\n", 1000);
        ASSERT_EQ(r.requests.size(), 1u) << token << ": " << r.error_reason;
        EXPECT_EQ(r.requests[0].method, method) << token;
    }
}

TEST(Http1ParserTest, UnknownMethodTokenIs501) {
    ParseRun r = parse("BREW /pot HTTP/1.1\r\nHost: h\r\n\r\n", 1000);
    EXPECT_EQ(r.error_status, 501) << r.error_reason;
}

TEST(Http1ParserTest, RepeatedHeaderKeepsTheLastValue) {
    ParseRun r = parse("GET / HTTP/1.1\r\nHost: h\r\nX-A: first\r\nx-a: second\r\n\r\n", 1000);
    ASSERT_EQ(r.requests.size(), 1u);
    EXPECT_EQ(r.requests[0].header("x-a"), "second");
}

TEST(Http1ParserTest, HeadersLiveInTheRequestNotTheInput) {
    Http1Parser p;
    std::string input = "GET /x HTTP/1.1\r\nHost: owned\r\n\r\n";
    size_t used = 0;
    ASSERT_EQ(p.feed(input, used), Event::MessageComplete);
    input.assign(input.size(), '#'); // the caller reuses its buffer
    EXPECT_EQ(p.request().headers.at("Host"), "owned");
    EXPECT_EQ(p.request().uri, "/x");
}

// A coroutine handler holds the request's storage (keep_alive()) past
// next(), while the connection parses the following request (#200).
TEST(Http1ParserTest, AHeldRequestSurvivesNext) {
    Http1Parser p;
    std::string input = "POST /first HTTP/1.1\r\nHost: one\r\nContent-Length: 5\r\n\r\nalpha"
                        "POST /second HTTP/1.1\r\nHost: two\r\nContent-Length: 4\r\n\r\nbeta";
    size_t used = 0;
    ASSERT_EQ(p.feed(input, used), Event::HeadersComplete);
    size_t more = 0;
    ASSERT_EQ(p.feed(std::string_view(input).substr(used), more), Event::MessageComplete);
    used += more;

    const orbit::http::HttpRequest& first = p.request();
    std::shared_ptr<void> held = first.keep_alive();
    ASSERT_TRUE(held);

    p.next();
    ASSERT_EQ(p.feed(std::string_view(input).substr(used), more), Event::HeadersComplete);
    used += more;
    ASSERT_EQ(p.feed(std::string_view(input).substr(used), more), Event::MessageComplete);

    EXPECT_NE(&p.request(), &first) << "the next request gets fresh storage";
    EXPECT_EQ(first.uri, "/first");
    EXPECT_EQ(first.headers.at("Host"), "one");
    EXPECT_EQ(first.body, "alpha");
    EXPECT_EQ(p.request().uri, "/second");
    EXPECT_EQ(p.request().headers.at("Host"), "two");
    EXPECT_EQ(p.request().body, "beta");
}

// Without a holder the storage is reused, as before.
TEST(Http1ParserTest, AnUnheldRequestIsReusedInPlace) {
    Http1Parser p;
    std::string input = "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /b HTTP/1.1\r\nHost: h\r\n\r\n";
    size_t used = 0;
    ASSERT_EQ(p.feed(input, used), Event::MessageComplete);
    const orbit::http::HttpRequest* first = &p.request();
    p.next();
    size_t more = 0;
    ASSERT_EQ(p.feed(std::string_view(input).substr(used), more), Event::MessageComplete);
    EXPECT_EQ(&p.request(), first);
    EXPECT_EQ(p.request().uri, "/b");
}

// --- Bodies ---

TEST(Http1ParserTest, ContentLengthBody) {
    for (const ParseRun& r : parse_all_ways("POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello world")) {
        ASSERT_EQ(r.bodies.size(), 1u) << events(r) << r.error_reason;
        EXPECT_EQ(r.bodies[0], "hello world");
        EXPECT_EQ(r.events.front(), Event::HeadersComplete) << "pauses at the headers first";
    }
}

TEST(Http1ParserTest, ChunkedBodyIsDecodedWithExtensionsAndTrailers) {
    const std::string req =
        "POST /c HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=1\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\nX-Trailer: t\r\n\r\n";
    for (const ParseRun& r : parse_all_ways(req)) {
        ASSERT_EQ(r.bodies.size(), 1u) << events(r) << r.error_reason;
        EXPECT_EQ(r.bodies[0], "hello world");
    }
}

TEST(Http1ParserTest, BodyHandlerStreamsInsteadOfBuffering) {
    Http1Parser p;
    const std::string req = "PUT /s HTTP/1.1\r\nHost: h\r\nContent-Length: 6\r\n\r\nabcdef";
    size_t used = 0;
    ASSERT_EQ(p.feed(req, used), Event::HeadersComplete);
    EXPECT_TRUE(p.has_body());
    EXPECT_FALSE(p.chunked());
    std::string streamed;
    p.set_body_handler([&](std::string_view piece) { streamed += piece; });
    std::string_view rest(req.data() + used, req.size() - used);
    for (char c : rest) { // one byte at a time
        size_t n = 0;
        p.feed(std::string_view(&c, 1), n);
    }
    EXPECT_EQ(streamed, "abcdef");
    EXPECT_TRUE(p.request().body.empty());
}

TEST(Http1ParserTest, ExpectContinueIsKnownBeforeTheBody) {
    Http1Parser p;
    const std::string head = "POST /u HTTP/1.1\r\nHost: h\r\nExpect: 100-Continue\r\nContent-Length: 3\r\n\r\n";
    size_t used = 0;
    ASSERT_EQ(p.feed(head, used), Event::HeadersComplete);
    EXPECT_EQ(used, head.size());
    EXPECT_TRUE(p.expect_continue());
    EXPECT_TRUE(p.has_body());
    EXPECT_EQ(p.feed("abc", used), Event::MessageComplete);
    EXPECT_EQ(p.request().body, "abc");
}

TEST(Http1ParserTest, OnlyRequestsWithABodyPauseAtTheHeaders) {
    for (const ParseRun& r : parse_all_ways("GET / HTTP/1.1\r\nHost: h\r\n\r\n")) {
        ASSERT_EQ(r.events.size(), 1u) << events(r);
        EXPECT_EQ(r.events[0], Event::MessageComplete);
    }
    for (const ParseRun& r : parse_all_ways("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n")) {
        ASSERT_EQ(r.events.size(), 1u) << "Content-Length: 0 is no body: " << events(r);
    }
}

TEST(Http1ParserTest, NoFramingHeadersMeansNoBody) {
    ParseRun r = parse("POST /n HTTP/1.1\r\nHost: h\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n", 1000);
    ASSERT_EQ(r.requests.size(), 2u) << r.error_reason;
    EXPECT_TRUE(r.bodies[0].empty());
    EXPECT_EQ(r.requests[1].method, HttpMethod::GET);
}

// --- Connections ---

TEST(Http1ParserTest, PipelinedRequestsAreSplitExactly) {
    const std::string req =
        "POST /one HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc"
        "POST /two HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nde\r\n0\r\n\r\n"
        "GET /three HTTP/1.1\r\nHost: h\r\n\r\n";
    for (const ParseRun& r : parse_all_ways(req)) {
        ASSERT_EQ(r.requests.size(), 3u) << events(r) << r.error_reason;
        EXPECT_EQ(r.requests[0].uri, "/one");
        EXPECT_EQ(r.bodies[0], "abc");
        EXPECT_EQ(r.requests[1].uri, "/two");
        EXPECT_EQ(r.bodies[1], "de");
        EXPECT_EQ(r.requests[2].uri, "/three");
    }
}

TEST(Http1ParserTest, KeepAliveFollowsVersionAndConnectionHeader) {
    auto keep_alive = [](const std::string& req) {
        Http1Parser p;
        size_t used = 0;
        p.feed(req, used);
        return p.keep_alive();
    };
    EXPECT_TRUE(keep_alive("GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    EXPECT_FALSE(keep_alive("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n"));
    EXPECT_FALSE(keep_alive("GET / HTTP/1.0\r\n\r\n"));
    EXPECT_TRUE(keep_alive("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"));
}

// An upgrade request ends at its headers; the bytes after it belong to the
// new protocol and are left unconsumed.
TEST(Http1ParserTest, UpgradeStopsAtTheHeaders) {
    const std::string head =
        "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    const std::string frame = "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58";
    Http1Parser p;
    std::string input = head + frame;
    size_t used = 0;
    ASSERT_EQ(p.feed(input, used), Event::MessageComplete) << "no body, so no pause at the headers";
    EXPECT_TRUE(p.upgrade());
    EXPECT_EQ(used, head.size()) << "the WebSocket frame is not consumed";
}

TEST(Http1ParserTest, DeclinedUpgradeKeepsParsingHttp) {
    ParseRun r = parse("GET /a HTTP/1.1\r\nHost: h\r\nUpgrade: h2c\r\nConnection: Upgrade\r\n\r\n"
                  "GET /b HTTP/1.1\r\nHost: h\r\n\r\n", 1000);
    ASSERT_EQ(r.requests.size(), 2u) << events(r) << r.error_reason;
    EXPECT_EQ(r.requests[1].uri, "/b");
}

// --- Limits, enforced as bytes arrive ---

TEST(Http1ParserTest, OverlongRequestLineIs431BeforeItsEnd) {
    Http1Parser::Limits limits;
    limits.max_request_line = 64;
    // No CRLF yet: it fails anyway.
    ParseRun r = parse("GET /" + std::string(200, 'a'), 1000, limits);
    EXPECT_EQ(r.error_status, 431) << r.error_reason;
}

TEST(Http1ParserTest, OversizedHeaderSectionIs431BeforeItsEnd) {
    Http1Parser::Limits limits;
    limits.max_header_bytes = 256;
    ParseRun r = parse("GET / HTTP/1.1\r\nHost: h\r\nX-Big: " + std::string(1000, 'v'), 1000, limits);
    EXPECT_EQ(r.error_status, 431) << r.error_reason;
    for (const ParseRun& each : parse_all_ways("GET / HTTP/1.1\r\nX-Big: " + std::string(1000, 'v') + "\r\n\r\n", limits)) {
        EXPECT_EQ(each.error_status, 431) << events(each);
    }
}

TEST(Http1ParserTest, TooManyHeaderFieldsIs431) {
    Http1Parser::Limits limits;
    limits.max_headers = 3;
    std::string req = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 4; ++i) req += "X-" + std::to_string(i) + ": v\r\n";
    EXPECT_EQ(parse(req + "\r\n", 1000, limits).error_status, 431);
    EXPECT_TRUE(parse("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", 1000, limits).error_status == 0);
}

TEST(Http1ParserTest, DeclaredBodyOverTheLimitIs413BeforeReadingIt) {
    Http1Parser::Limits limits;
    limits.max_body_size = 10;
    Http1Parser p(limits);
    size_t used = 0;
    EXPECT_EQ(p.feed("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n", used), Event::Error);
    EXPECT_EQ(p.error_status(), 413);
}

TEST(Http1ParserTest, ChunkedBodyOverTheLimitIs413) {
    Http1Parser::Limits limits;
    limits.max_body_size = 10;
    const std::string req = "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                            "6\r\nabcdef\r\n6\r\nghijkl\r\n0\r\n\r\n";
    for (const ParseRun& r : parse_all_ways(req, limits)) EXPECT_EQ(r.error_status, 413) << events(r);
    // Exactly at the limit is fine.
    EXPECT_EQ(parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n0123456789", 1000, limits).bodies.at(0),
              "0123456789");
}

TEST(Http1ParserTest, BodyLimitCanBeChosenPerRequest) {
    Http1Parser::Limits limits;
    limits.max_body_size = 4;
    Http1Parser p(limits);
    p.set_body_limit([](const orbit::http::HttpRequest& r) -> size_t {
        return r.uri == "/upload" ? SIZE_MAX : 4;
    });
    const std::string big = "POST /upload HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n0123456789";
    size_t used = 0;
    std::string_view rest = big;
    ASSERT_EQ(p.feed(rest, used), Event::HeadersComplete);
    rest.remove_prefix(used);
    ASSERT_EQ(p.feed(rest, used), Event::MessageComplete);
    EXPECT_EQ(p.request().body, "0123456789");
    p.next();
    // The limit is chosen again for the next request on the connection.
    EXPECT_EQ(p.feed("POST /other HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n", used), Event::Error);
    EXPECT_EQ(p.error_status(), 413);
}

TEST(Http1ParserTest, ExpectContinueIsCaseInsensitiveAndTrimmed) {
    auto expects = [](const std::string& headers) {
        Http1Parser p;
        size_t used = 0;
        p.feed("POST / HTTP/1.1\r\nHost: x\r\n" + headers + "Content-Length: 5\r\n\r\n", used);
        return p.expect_continue();
    };
    EXPECT_TRUE(expects("Expect: 100-continue\r\n"));
    EXPECT_TRUE(expects("expect:  100-Continue \r\n"));
    EXPECT_FALSE(expects(""));
    EXPECT_FALSE(expects("Expect: something-else\r\n"));
}

// Framing is read from the framing headers only, however they are spelt.
TEST(Http1ParserTest, FramingHeaderSpellings) {
    const char* accepted[] = {
        "POST / HTTP/1.1\r\nHost: h\r\ncontent-LENGTH:   3 \r\n\r\nabc",
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: CHUNKED\r\n\r\n3\r\nabc\r\n0\r\n\r\n",
        // Text that looks like framing inside another header is just text.
        "POST / HTTP/1.1\r\nHost: h\r\nX-Note: content-length: 999\r\nContent-Length: 3\r\n\r\nabc",
    };
    for (const char* req : accepted) {
        ParseRun r = parse(req, 1000);
        ASSERT_EQ(r.bodies.size(), 1u) << req << " -> " << r.error_reason;
        EXPECT_EQ(r.bodies[0], "abc") << req;
    }
}

TEST(Http1ParserTest, MalformedContentLengthAndTransferEncodingAre400) {
    const char* bad[] = {
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: +5\r\n\r\nabcde",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: \r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: abc\r\n\r\nA",
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: identity\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding\t: chunked\r\n\r\n",
        // a chunk size that overflows
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nfffffffffffffffff\r\n",
        // an empty chunk-size line
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n\r\n",
    };
    for (const char* req : bad) {
        for (const ParseRun& r : parse_all_ways(req)) {
            EXPECT_EQ(r.error_status, 400) << "accepted: " << req << " -> " << events(r);
        }
    }
}

TEST(Http1ParserTest, IncompleteChunkedBodyWaitsForMore) {
    for (const char* partial : {"5\r\nhel", "5\r\nhello\r\n", "0\r\n"}) {
        Http1Parser p;
        std::string req = std::string("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n") + partial;
        size_t used = 0;
        std::string_view rest = req;
        ASSERT_EQ(p.feed(rest, used), Event::HeadersComplete);
        rest.remove_prefix(used);
        EXPECT_EQ(p.feed(rest, used), Event::NeedMore) << partial;
    }
}

// --- Strict framing (request smuggling) ---

TEST(Http1ParserTest, AmbiguousOrMalformedFramingIs400) {
    const char* bad[] = {
        // Content-Length and Transfer-Encoding together
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        // conflicting Content-Lengths
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
        // repeated identical Content-Length (rejected; the old parser accepted it)
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
        // non-numeric and signed Content-Length
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3a\r\n\r\nabc",
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n",
        // chunked not the final coding
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
        // whitespace before the colon
        "GET / HTTP/1.1\r\nHost : h\r\n\r\n",
        // obs-fold
        "GET / HTTP/1.1\r\nHost: h\r\nX-Folded: a\r\n b\r\n\r\n",
        // a header line without a colon
        "GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n",
        // bare LF line endings
        "GET / HTTP/1.1\nHost: h\n\n",
        // malformed chunk size and missing CRLF after a chunk
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nabc\r\n0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX0\r\n\r\n",
        // invalid percent-encoding in the path
        "GET /%zz HTTP/1.1\r\nHost: h\r\n\r\n",
        // a version that is not HTTP/1.x syntax
        "GET / HTTX/1.1\r\nHost: h\r\n\r\n",
    };
    for (const char* req : bad) {
        for (const ParseRun& r : parse_all_ways(req)) {
            EXPECT_EQ(r.error_status, 400) << "accepted: " << req << " -> " << events(r);
        }
    }
}

TEST(Http1ParserTest, AnErrorIsFinal) {
    Http1Parser p;
    size_t used = 0;
    ASSERT_EQ(p.feed("GET / HTTP/1.1\r\nHost : h\r\n\r\n", used), Event::Error);
    EXPECT_EQ(p.feed("GET / HTTP/1.1\r\nHost: h\r\n\r\n", used), Event::Error);
    EXPECT_EQ(used, 0u);
    p.next(); // no effect after an error
    EXPECT_EQ(p.feed("GET / HTTP/1.1\r\nHost: h\r\n\r\n", used), Event::Error);
}

TEST(Http1ParserTest, FeedingAfterMessageCompleteNeedsNext) {
    Http1Parser p;
    size_t used = 0;
    const std::string two = "GET /1 HTTP/1.1\r\nHost: h\r\n\r\nGET /2 HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string_view rest = two;
    ASSERT_EQ(p.feed(rest, used), Event::MessageComplete);
    rest.remove_prefix(used);
    EXPECT_EQ(p.feed(rest, used), Event::MessageComplete) << "still the first request";
    EXPECT_EQ(used, 0u);
    EXPECT_EQ(p.request().uri, "/1");
    p.next();
    ASSERT_EQ(p.feed(rest, used), Event::MessageComplete);
    EXPECT_EQ(p.request().uri, "/2");
}
