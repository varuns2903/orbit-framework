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

using namespace http;
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
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
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
             middleware::StaticFilesOptions options = {}) {
        auto mw = middleware::static_files(pub.string(), options);
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
    middleware::StaticFilesOptions opts;
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
    middleware::StaticFilesOptions no_index;
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

    middleware::StaticFilesOptions opts;
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
    EXPECT_EQ(middleware::mime_type_for_extension(".woff2"), "font/woff2");
    EXPECT_EQ(middleware::mime_type_for_extension(".wasm"), "application/wasm");
    EXPECT_EQ(middleware::mime_type_for_extension(".mjs"), "text/javascript; charset=utf-8");
    EXPECT_EQ(middleware::mime_type_for_extension(".ICO"), "image/x-icon");
    EXPECT_EQ(middleware::mime_type_for_extension(".unknown"), "application/octet-stream");
    EXPECT_EQ(middleware::mime_type_for_extension(""), "application/octet-stream");
}
