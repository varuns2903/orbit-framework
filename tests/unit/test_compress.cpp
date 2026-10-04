#include <gtest/gtest.h>
#include <orbit/middleware/Compress.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <zlib.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace http;

namespace {

// Runs registered interceptors on send(), as Connection does.
class InterceptingWriter : public ResponseWriter {
public:
    HttpResponse last;
    std::vector<std::function<void(HttpResponse&)>> interceptors;

    void send(HttpResponse&& response) override {
        for (auto& i : interceptors) i(response);
        last = std::move(response);
    }
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override {
        interceptors.push_back(std::move(interceptor));
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

std::string gunzip(const std::string& in) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    inflateInit2(&zs, 15 | 16);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out(1 << 16, '\0');
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(out.size());
    inflate(&zs, Z_FINISH);
    out.resize(out.size() - zs.avail_out);
    inflateEnd(&zs);
    return out;
}

const std::string kBody(2000, 'x');

// Runs a request through compress() and a handler that sends `res`.
HttpResponse run(const char* accept_encoding, HttpResponse res) {
    auto mw = middleware::compress();
    HttpRequest req;
    if (accept_encoding) req.headers["Accept-Encoding"] = accept_encoding;
    auto writer = std::make_shared<InterceptingWriter>();
    EXPECT_TRUE(mw(req, writer));
    writer->send(std::move(res));
    return std::move(writer->last);
}

HttpResponse text_response(const std::string& body = kBody) {
    HttpResponse res;
    res.set_body(body, "text/plain");
    return res;
}

} // namespace

TEST(CompressTest, AcceptEncodingParsing) {
    EXPECT_TRUE(middleware::accepts_gzip("gzip"));
    EXPECT_TRUE(middleware::accepts_gzip("deflate, GZIP;q=0.5"));
    EXPECT_TRUE(middleware::accepts_gzip("x-gzip"));
    EXPECT_TRUE(middleware::accepts_gzip("*"));
    EXPECT_TRUE(middleware::accepts_gzip("br, *;q=0.1"));
    EXPECT_FALSE(middleware::accepts_gzip("gzip;q=0"));
    EXPECT_FALSE(middleware::accepts_gzip("gzip; q=0.000"));
    EXPECT_FALSE(middleware::accepts_gzip("*, gzip;q=0")); // explicit entry beats the wildcard
    EXPECT_FALSE(middleware::accepts_gzip("*;q=0"));
    EXPECT_FALSE(middleware::accepts_gzip("br, deflate"));
    EXPECT_FALSE(middleware::accepts_gzip("gzipped"));
    EXPECT_FALSE(middleware::accepts_gzip(""));
    EXPECT_FALSE(middleware::accepts_gzip("gzip;q=abc")); // malformed q counts as 0
}

TEST(CompressTest, CompressesAndSetsVary) {
    HttpResponse res = run("gzip, deflate", text_response());
    EXPECT_EQ(res.headers["Content-Encoding"], "gzip");
    EXPECT_EQ(res.headers["Vary"], "Accept-Encoding");
    EXPECT_EQ(res.headers["Content-Length"], std::to_string(res.body.size()));
    EXPECT_EQ(gunzip(res.body), kBody);
}

TEST(CompressTest, QZeroMeansNoGzip) {
    HttpResponse res = run("gzip;q=0, identity", text_response());
    EXPECT_EQ(res.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(res.body, kBody);
    EXPECT_EQ(res.headers["Vary"], "Accept-Encoding");
}

TEST(CompressTest, PlainResponsesStillCarryVary) {
    HttpResponse res = run(nullptr, text_response());
    EXPECT_EQ(res.body, kBody);
    EXPECT_EQ(res.headers["Vary"], "Accept-Encoding");
}

TEST(CompressTest, ExistingVaryIsExtended) {
    HttpResponse in = text_response();
    in.headers["Vary"] = "Origin";
    HttpResponse res = run("gzip", std::move(in));
    EXPECT_EQ(res.headers["Vary"], "Origin, Accept-Encoding");

    HttpResponse already = text_response();
    already.headers["Vary"] = "accept-encoding";
    EXPECT_EQ(run("gzip", std::move(already)).headers["Vary"], "accept-encoding");
}

TEST(CompressTest, AlreadyEncodedBodiesAreNotTouched) {
    HttpResponse in = text_response();
    in.headers["Content-Encoding"] = "br";
    HttpResponse res = run("gzip", std::move(in));
    EXPECT_EQ(res.headers["Content-Encoding"], "br");
    EXPECT_EQ(res.body, kBody);
}

TEST(CompressTest, NoTransformIsRespected) {
    HttpResponse in = text_response();
    in.headers["Cache-Control"] = "public, no-transform";
    HttpResponse res = run("gzip", std::move(in));
    EXPECT_EQ(res.headers.count("Content-Encoding"), 0u);
}

TEST(CompressTest, StrongEtagBecomesWeak) {
    HttpResponse strong = text_response();
    strong.headers["ETag"] = "\"abc\"";
    EXPECT_EQ(run("gzip", std::move(strong)).headers["ETag"], "W/\"abc\"");

    HttpResponse weak = text_response();
    weak.headers["ETag"] = "W/\"abc\"";
    EXPECT_EQ(run("gzip", std::move(weak)).headers["ETag"], "W/\"abc\"");

    // Not compressed: the strong tag stays.
    HttpResponse plain = text_response();
    plain.headers["ETag"] = "\"abc\"";
    EXPECT_EQ(run("gzip;q=0", std::move(plain)).headers["ETag"], "\"abc\"");
}

TEST(CompressTest, SkipsSmallAndPrecompressedBodies) {
    HttpResponse small = run("gzip", text_response("tiny"));
    EXPECT_EQ(small.headers.count("Content-Encoding"), 0u);

    HttpResponse png;
    png.set_body(kBody, "image/png");
    EXPECT_EQ(run("gzip", std::move(png)).headers.count("Content-Encoding"), 0u);

    HttpResponse svg;
    svg.set_body(kBody, "image/svg+xml"); // text, compresses well
    EXPECT_EQ(run("gzip", std::move(svg)).headers["Content-Encoding"], "gzip");
}

TEST(CompressTest, IncompressibleBodyIsSentAsIs) {
    // Pseudo-random bytes grow under gzip; the original is kept.
    std::string noise;
    uint32_t x = 12345;
    for (int i = 0; i < 4000; ++i) {
        x = x * 1103515245u + 12345u;
        noise.push_back(static_cast<char>(x >> 24));
    }
    HttpResponse in;
    in.set_body(noise, "application/octet-stream");
    HttpResponse res = run("gzip", std::move(in));
    EXPECT_EQ(res.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(res.body, noise);
}
