#include <gtest/gtest.h>
#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace orbit::http;
namespace fs = std::filesystem;

namespace {

class StaticMockWriter : public ResponseWriter {
public:
    HttpResponse last;
    int sends = 0;
    void send(HttpResponse&& response) override {
        last = std::move(response);
        ++sends;
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

} // namespace

class StaticFilesTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path pub;

    void SetUp() override {
        root = fs::temp_directory_path() /
               ("orbit_static_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(root);
        pub = root / "public";
        write_file(pub / "index.html", "<h1>home</h1>");
        write_file(pub / "logo.SVG", "<svg/>");
        write_file(pub / "digits.txt", "0123456789");
        write_file(pub / "docs" / "index.html", "docs");
        write_file(pub / ".env", "SECRET=1");
        write_file(pub / ".git" / "config", "[core]");
        write_file(root / "secret.txt", "outside");
        write_file(root / "public-old" / "leak.txt", "sibling");
    }

    void TearDown() override { fs::remove_all(root); }

    // Returns true if the middleware let the request continue down the pipeline.
    bool run(const std::string& uri, StaticMockWriter& writer, HttpMethod method = HttpMethod::GET,
             std::initializer_list<std::pair<std::string_view, std::string_view>> headers = {},
             orbit::middleware::StaticFilesOptions options = {}) {
        auto mw = orbit::middleware::static_files(pub.string(), options);
        HttpRequest req;
        req.method = method;
        req.uri = uri;
        for (const auto& [k, v] : headers) req.headers[k] = v;
        auto shared = std::shared_ptr<StaticMockWriter>(&writer, [](StaticMockWriter*) {});
        return mw(req, shared);
    }
};

TEST_F(StaticFilesTest, ServesFileWithTypeAndValidators) {
    StaticMockWriter w;
    EXPECT_FALSE(run("/logo.SVG", w));
    EXPECT_EQ(w.last.status_code, HttpStatus::OK);
    EXPECT_EQ(w.last.headers["Content-Type"], "image/svg+xml");
    EXPECT_EQ(w.last.headers["Content-Length"], "6");
    EXPECT_EQ(w.last.headers["Accept-Ranges"], "bytes");
    EXPECT_EQ(w.last.headers["Cache-Control"], "no-cache");
    EXPECT_FALSE(w.last.headers["ETag"].empty());
    EXPECT_NE(w.last.headers["Last-Modified"].find(" GMT"), std::string::npos);
    EXPECT_NE(w.last.file_fd, -1);
}

TEST_F(StaticFilesTest, MaxAgeSetsCacheControl) {
    StaticMockWriter w;
    orbit::middleware::StaticFilesOptions opts;
    opts.max_age = std::chrono::hours(1);
    EXPECT_FALSE(run("/digits.txt", w, HttpMethod::GET, {}, opts));
    EXPECT_EQ(w.last.headers["Cache-Control"], "public, max-age=3600");
}

TEST_F(StaticFilesTest, HeadIsServed) {
    StaticMockWriter w;
    EXPECT_FALSE(run("/digits.txt", w, HttpMethod::HEAD));
    EXPECT_EQ(w.last.status_code, HttpStatus::OK);
    EXPECT_EQ(w.last.headers["Content-Length"], "10");
}

TEST_F(StaticFilesTest, OtherMethodsContinue) {
    StaticMockWriter w;
    EXPECT_TRUE(run("/digits.txt", w, HttpMethod::POST));
    EXPECT_EQ(w.sends, 0);
}

TEST_F(StaticFilesTest, DirectoriesServeTheirIndex) {
    StaticMockWriter w;
    EXPECT_FALSE(run("/", w));
    EXPECT_EQ(w.last.headers["Content-Type"], "text/html; charset=utf-8");
    EXPECT_EQ(w.last.headers["Content-Length"], "13");

    StaticMockWriter w2;
    EXPECT_FALSE(run("/docs/", w2));
    EXPECT_EQ(w2.last.headers["Content-Length"], "4");

    StaticMockWriter w3;
    orbit::middleware::StaticFilesOptions no_index;
    no_index.index.clear();
    EXPECT_TRUE(run("/docs/", w3, HttpMethod::GET, {}, no_index));
}

TEST_F(StaticFilesTest, MissingFilesContinue) {
    StaticMockWriter w;
    EXPECT_TRUE(run("/nope.txt", w));
    EXPECT_EQ(w.sends, 0);
}

TEST_F(StaticFilesTest, DotfilesAreHiddenByDefault) {
    StaticMockWriter w;
    EXPECT_TRUE(run("/.env", w));
    EXPECT_TRUE(run("/.git/config", w));
    EXPECT_EQ(w.sends, 0);

    orbit::middleware::StaticFilesOptions opts;
    opts.serve_dotfiles = true;
    EXPECT_FALSE(run("/.env", w, HttpMethod::GET, {}, opts));
    EXPECT_EQ(w.last.status_code, HttpStatus::OK);
}

TEST_F(StaticFilesTest, TraversalIsForbidden) {
    StaticMockWriter w;
    EXPECT_FALSE(run("/../secret.txt", w));
    EXPECT_EQ(w.last.status_code, HttpStatus::Forbidden);

    // "public-old" shares a string prefix with "public" but is a different directory.
    StaticMockWriter w2;
    EXPECT_FALSE(run("/../public-old/leak.txt", w2));
    EXPECT_EQ(w2.last.status_code, HttpStatus::Forbidden);
}

#ifndef _WIN32
TEST_F(StaticFilesTest, SymlinkOutOfTheDirectoryIsForbidden) {
    fs::create_symlink(root / "secret.txt", pub / "link.txt");
    StaticMockWriter w;
    EXPECT_FALSE(run("/link.txt", w));
    EXPECT_EQ(w.last.status_code, HttpStatus::Forbidden);
}
#endif

TEST_F(StaticFilesTest, IfNoneMatchListGives304) {
    StaticMockWriter first;
    run("/digits.txt", first);
    std::string etag = first.last.headers["ETag"];
    ASSERT_FALSE(etag.empty());

    std::string list = "\"other\", W/" + etag;
    StaticMockWriter w;
    EXPECT_FALSE(run("/digits.txt", w, HttpMethod::GET, {{"If-None-Match", list}}));
    EXPECT_EQ(w.last.status_code, HttpStatus::NotModified);
    EXPECT_EQ(w.last.file_fd, -1);

    StaticMockWriter star;
    run("/digits.txt", star, HttpMethod::GET, {{"If-None-Match", "*"}});
    EXPECT_EQ(star.last.status_code, HttpStatus::NotModified);

    StaticMockWriter miss;
    run("/digits.txt", miss, HttpMethod::GET, {{"If-None-Match", "\"other\""}});
    EXPECT_EQ(miss.last.status_code, HttpStatus::OK);
}

TEST_F(StaticFilesTest, IfModifiedSinceGives304) {
    StaticMockWriter first;
    run("/digits.txt", first);
    std::string last_modified = first.last.headers["Last-Modified"];

    StaticMockWriter w;
    run("/digits.txt", w, HttpMethod::GET, {{"If-Modified-Since", last_modified}});
    EXPECT_EQ(w.last.status_code, HttpStatus::NotModified);

    StaticMockWriter old;
    run("/digits.txt", old, HttpMethod::GET, {{"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT"}});
    EXPECT_EQ(old.last.status_code, HttpStatus::OK);

    StaticMockWriter junk;
    run("/digits.txt", junk, HttpMethod::GET, {{"If-Modified-Since", "yesterday"}});
    EXPECT_EQ(junk.last.status_code, HttpStatus::OK);
}

TEST_F(StaticFilesTest, ByteRanges) {
    struct Case {
        const char* range;
        const char* content_range;
        off_t offset;
        off_t end;
    };
    for (const Case& c : {Case{"bytes=2-5", "bytes 2-5/10", 2, 6},
                          Case{"bytes=5-", "bytes 5-9/10", 5, 10},
                          Case{"bytes=-3", "bytes 7-9/10", 7, 10},
                          Case{"bytes=8-100", "bytes 8-9/10", 8, 10}}) {
        StaticMockWriter w;
        EXPECT_FALSE(run("/digits.txt", w, HttpMethod::GET, {{"Range", c.range}}));
        EXPECT_EQ(w.last.status_code, HttpStatus::PartialContent) << c.range;
        EXPECT_EQ(w.last.headers["Content-Range"], c.content_range) << c.range;
        EXPECT_EQ(w.last.headers["Content-Length"], std::to_string(c.end - c.offset)) << c.range;
        EXPECT_EQ(w.last.file_offset, c.offset) << c.range;
        EXPECT_EQ(w.last.file_size, c.end) << c.range;
    }
}

TEST_F(StaticFilesTest, UnsatisfiableRangeGives416) {
    for (const char* range : {"bytes=10-", "bytes=-0"}) {
        StaticMockWriter w;
        EXPECT_FALSE(run("/digits.txt", w, HttpMethod::GET, {{"Range", range}}));
        EXPECT_EQ(w.last.status_code, HttpStatus::RangeNotSatisfiable) << range;
        EXPECT_EQ(w.last.headers["Content-Range"], "bytes */10") << range;
    }
}

TEST_F(StaticFilesTest, UnsupportedRangesSendTheWholeFile) {
    for (const char* range : {"bytes=0-1,4-5", "items=0-1", "bytes=5-2", "bytes=abc"}) {
        StaticMockWriter w;
        run("/digits.txt", w, HttpMethod::GET, {{"Range", range}});
        EXPECT_EQ(w.last.status_code, HttpStatus::OK) << range;
        EXPECT_EQ(w.last.headers["Content-Length"], "10") << range;
    }

    // A stale If-Range means the client's partial copy is outdated.
    StaticMockWriter w;
    run("/digits.txt", w, HttpMethod::GET, {{"Range", "bytes=0-1"}, {"If-Range", "\"stale\""}});
    EXPECT_EQ(w.last.status_code, HttpStatus::OK);
}

TEST(StaticFilesMimeTest, KnowsCommonWebTypes) {
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(".woff2"), "font/woff2");
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(".wasm"), "application/wasm");
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(".mjs"), "text/javascript; charset=utf-8");
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(".ICO"), "image/x-icon");
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(".unknown"), "application/octet-stream");
    EXPECT_EQ(orbit::middleware::mime_type_for_extension(""), "application/octet-stream");
}

// --- Mounting under a URL prefix (#199) ---

TEST_F(StaticFilesTest, MountServesTheDirectoryUnderItsPrefix) {
    orbit::middleware::StaticFilesOptions mounted;
    mounted.mount = "/files/"; // a trailing slash is ignored
    StaticMockWriter w1;
    EXPECT_FALSE(run("/files/digits.txt", w1, HttpMethod::GET, {}, mounted));
    EXPECT_EQ(w1.last.headers.at("Content-Length"), "10"); // digits.txt

    StaticMockWriter w2;
    EXPECT_FALSE(run("/files", w2, HttpMethod::GET, {}, mounted)) << "the prefix itself is the directory";
    EXPECT_EQ(w2.last.headers.at("Content-Length"), "13"); // index.html

    StaticMockWriter w3;
    EXPECT_FALSE(run("/files/docs/", w3, HttpMethod::GET, {}, mounted));
    EXPECT_EQ(w3.last.headers.at("Content-Length"), "4"); // docs/index.html
}

TEST_F(StaticFilesTest, MountIgnoresRequestsOutsideThePrefix) {
    orbit::middleware::StaticFilesOptions mounted;
    mounted.mount = "/files";
    for (const char* uri : {"/digits.txt", "/filesdigits.txt", "/other/files/digits.txt", "/"}) {
        StaticMockWriter w;
        EXPECT_TRUE(run(uri, w, HttpMethod::GET, {}, mounted)) << uri;
        EXPECT_EQ(w.sends, 0) << uri;
    }
}

TEST_F(StaticFilesTest, MountStillBlocksTraversalAndDotfiles) {
    orbit::middleware::StaticFilesOptions mounted;
    mounted.mount = "/files";
    StaticMockWriter w1;
    EXPECT_FALSE(run("/files/../secret.txt", w1, HttpMethod::GET, {}, mounted));
    EXPECT_EQ(w1.last.status_code, HttpStatus::Forbidden);

    StaticMockWriter w2;
    EXPECT_TRUE(run("/files/.env", w2, HttpMethod::GET, {}, mounted));
    EXPECT_EQ(w2.sends, 0);
}

TEST_F(StaticFilesTest, WithoutFallthroughAMissIsA404Here) {
    orbit::middleware::StaticFilesOptions strict;
    strict.mount = "/files";
    strict.fallthrough = false;
    for (const char* uri : {"/files/missing.txt", "/files/.env", "/files/docs/nope"}) {
        StaticMockWriter w;
        EXPECT_FALSE(run(uri, w, HttpMethod::GET, {}, strict)) << uri;
        EXPECT_EQ(w.last.status_code, HttpStatus::NotFound) << uri;
        EXPECT_EQ(w.last.body.find("SECRET"), std::string::npos);
    }
    // Outside the mount it still steps aside.
    StaticMockWriter outside;
    EXPECT_TRUE(run("/api/items", outside, HttpMethod::GET, {}, strict));
    EXPECT_EQ(outside.sends, 0);
    // And a found file is served as usual.
    StaticMockWriter found;
    EXPECT_FALSE(run("/files/digits.txt", found, HttpMethod::GET, {}, strict));
    EXPECT_EQ(found.last.headers.at("Content-Length"), "10");
}

// --- Precompressed siblings (#191) ---

TEST_F(StaticFilesTest, PrecompressedSiblingIsServedWhenAccepted) {
    write_file(pub / "app.js", std::string(1000, 'a'));
    write_file(pub / "app.js.br", "BR");
    write_file(pub / "app.js.gz", "GZIP");
    orbit::middleware::StaticFilesOptions opts;
    opts.precompressed = true;

    StaticMockWriter br;
    EXPECT_FALSE(run("/app.js", br, HttpMethod::GET, {{"Accept-Encoding", "gzip, br"}}, opts));
    EXPECT_EQ(br.last.headers.at("Content-Encoding"), "br");
    EXPECT_EQ(br.last.headers.at("Content-Length"), "2");
    EXPECT_EQ(br.last.headers.at("Content-Type").find("javascript") != std::string::npos, true)
        << br.last.headers.at("Content-Type");
    EXPECT_EQ(br.last.headers.at("Vary"), "Accept-Encoding");

    StaticMockWriter gz;
    EXPECT_FALSE(run("/app.js", gz, HttpMethod::GET, {{"Accept-Encoding", "gzip"}}, opts));
    EXPECT_EQ(gz.last.headers.at("Content-Encoding"), "gzip");
    EXPECT_EQ(gz.last.headers.at("Content-Length"), "4");
    EXPECT_NE(gz.last.headers.at("ETag"), br.last.headers.at("ETag")) << "an ETag per encoding";

    // Not accepted (or zstd only, which has no sibling): the plain file, still with Vary.
    for (const char* accept : {"", "identity", "zstd"}) {
        StaticMockWriter plain;
        EXPECT_FALSE(run("/app.js", plain, HttpMethod::GET, {{"Accept-Encoding", accept}}, opts)) << accept;
        EXPECT_EQ(plain.last.headers.count("Content-Encoding"), 0u) << accept;
        EXPECT_EQ(plain.last.headers.at("Content-Length"), "1000") << accept;
        EXPECT_EQ(plain.last.headers.at("Vary"), "Accept-Encoding") << accept;
    }
}

TEST_F(StaticFilesTest, PrecompressedIsOffByDefaultAndLeavesFilesWithoutSiblingsAlone) {
    write_file(pub / "app.js", "plain");
    write_file(pub / "app.js.br", "BR");
    StaticMockWriter off;
    EXPECT_FALSE(run("/app.js", off, HttpMethod::GET, {{"Accept-Encoding", "br"}}));
    EXPECT_EQ(off.last.headers.count("Content-Encoding"), 0u);

    orbit::middleware::StaticFilesOptions opts;
    opts.precompressed = true;
    StaticMockWriter none;
    EXPECT_FALSE(run("/digits.txt", none, HttpMethod::GET, {{"Accept-Encoding", "br, gzip"}}, opts));
    EXPECT_EQ(none.last.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(none.last.headers.count("Vary"), 0u) << "no variants, nothing varies";
}

// A sibling that is a symlink out of the directory is not served.
TEST_F(StaticFilesTest, PrecompressedSiblingMustStayInside) {
    write_file(pub / "app.js", "plain");
    std::error_code ec;
    fs::create_symlink(root / "secret.txt", pub / "app.js.gz", ec);
    if (ec) GTEST_SKIP() << "symlinks unavailable: " << ec.message();
    orbit::middleware::StaticFilesOptions opts;
    opts.precompressed = true;
    StaticMockWriter w;
    EXPECT_FALSE(run("/app.js", w, HttpMethod::GET, {{"Accept-Encoding", "gzip"}}, opts));
    EXPECT_EQ(w.last.headers.count("Content-Encoding"), 0u);
    EXPECT_EQ(w.last.headers.at("Content-Length"), "5");
}
