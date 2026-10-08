#include <gtest/gtest.h>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace orbit::http;

TEST(MultipartTest, BasicParsing) {
    std::string boundary = "boundary123";
    MultipartForm form;
    
    auto on_field = [&](const std::string& name, const std::string& value) {
        form.fields[name] = value;
    };
    
    auto on_file = [&](const std::string& name, const std::string& filename, const std::string& content_type, const std::string& data) {
        MultipartFile file;
        file.name = name;
        file.filename = filename;
        file.content_type = content_type;
        // `data` here is the temporary filepath!
        EXPECT_EQ(name, "file1");
        EXPECT_EQ(filename, "a.txt");
        EXPECT_EQ(content_type, "text/plain");
        // A private per-process directory, never the old shared /tmp/orbit_uploads.
        EXPECT_EQ(data.find("orbit_uploads"), std::string::npos) << data;
        EXPECT_NE(data.find("orbit-uploads-"), std::string::npos) << data;
        
        // Let's actually verify the file contents
        std::ifstream ifs(data);
        std::string file_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        EXPECT_EQ(file_content, "file content");
        
        form.files.push_back(file);
    };
    
    MultipartStreamParser parser(boundary, on_field, on_file);
    
    std::string body = 
        "--boundary123\r\n"
        "Content-Disposition: form-data; name=\"text1\"\r\n"
        "\r\n"
        "hello world\r\n"
        "--boundary123\r\n"
        "Content-Disposition: form-data; name=\"file1\"; filename=\"a.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file content\r\n"
        "--boundary123--\r\n";
        
    parser.feed(body);
    
    EXPECT_EQ(form.fields.count("text1"), 1);
    EXPECT_EQ(form.fields["text1"], "hello world");
    EXPECT_EQ(form.files.size(), 1);
}

TEST(MultipartTest, PartialFeed) {
    std::string boundary = "boundary123";
    MultipartForm form;
    
    MultipartStreamParser parser(boundary, 
        [&](const std::string& name, const std::string& value) { form.fields[name] = value; },
        [](const std::string&, const std::string&, const std::string&, const std::string&) {}
    );
    
    std::string chunk1 = 
        "--boundary123\r\n"
        "Content-Disposition: form-data; name=\"foo\"\r\n"
        "\r\n"
        "bar";
        
    std::string chunk2 = 
        "\r\n"
        "--boundary123--\r\n";
        
    parser.feed(chunk1);
    EXPECT_EQ(form.fields.count("foo"), 0); 
    
    parser.feed(chunk2);
    EXPECT_EQ(form.fields.count("foo"), 1);
    EXPECT_EQ(form.fields["foo"], "bar");
}

namespace {

std::string file_part(const std::string& name, const std::string& filename, const std::string& content) {
    return "--b\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" + filename +
           "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + content + "\r\n";
}

std::string field_part(const std::string& name, const std::string& value) {
    return "--b\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
}

const std::string kEnd = "--b--\r\n";

size_t files_in(const std::filesystem::path& dir) {
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++n;
    }
    return n;
}

// A private scratch directory per test.
std::filesystem::path scratch_dir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / (std::string("orbit_mp_test_") + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST(MultipartTest, UploadsGetUnpredictablePrivateFiles) {
    std::vector<std::string> paths;
    MultipartStreamParser parser("b", nullptr,
        [&](const std::string&, const std::string&, const std::string&, const std::string& path) { paths.push_back(path); });
    parser.feed(file_part("a", "1.txt", "one") + file_part("b", "2.txt", "two") + kEnd);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_NE(paths[0], paths[1]);
    // 128-bit random names: 32 hex characters after "upload-".
    std::string leaf = std::filesystem::path(paths[0]).filename().string();
    EXPECT_EQ(leaf.rfind("upload-", 0), 0u);
    EXPECT_EQ(leaf.size(), 7u + 32u);
#ifndef _WIN32
    struct stat st {};
    ASSERT_EQ(::stat(paths[0].c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u) << "upload readable by other users";
    ASSERT_EQ(::stat(std::filesystem::path(paths[0]).parent_path().c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0700u) << "upload directory open to other users";
#endif
    for (const auto& p : paths) std::filesystem::remove(p);
}

TEST(MultipartTest, FileLimitFailsAndDeletesThePartialFile) {
    auto dir = scratch_dir("file_limit");
    MultipartLimits limits;
    limits.max_file_size = 10;
    limits.upload_dir = dir.string();
    bool reported = false;
    MultipartStreamParser parser("b", nullptr,
        [&](const std::string&, const std::string&, const std::string&, const std::string&) { reported = true; }, limits);
    EXPECT_FALSE(parser.feed(file_part("f", "big.bin", std::string(100, 'x')) + kEnd));
    EXPECT_TRUE(parser.failed());
    EXPECT_NE(parser.error().find("max_file_size"), std::string::npos);
    EXPECT_FALSE(reported);
    EXPECT_EQ(files_in(dir), 0u);
    EXPECT_FALSE(parser.feed(field_part("later", "ignored"))); // nothing after a failure
    std::filesystem::remove_all(dir);
}

TEST(MultipartTest, FieldTotalAndPartLimits) {
    MultipartLimits field;
    field.max_field_size = 5;
    MultipartStreamParser p1("b", [](const std::string&, const std::string&) {}, nullptr, field);
    p1.feed(field_part("note", "far too long") + kEnd);
    EXPECT_TRUE(p1.failed());

    MultipartLimits total;
    total.max_total_size = 50;
    MultipartStreamParser p2("b", [](const std::string&, const std::string&) {}, nullptr, total);
    p2.feed(field_part("a", std::string(40, 'a')));
    p2.feed(field_part("b", std::string(40, 'b')));
    EXPECT_TRUE(p2.failed());

    MultipartLimits parts;
    parts.max_parts = 3;
    int seen = 0;
    MultipartStreamParser p3("b", [&](const std::string&, const std::string&) { ++seen; }, nullptr, parts);
    std::string body;
    for (int i = 0; i < 10; ++i) body += field_part("f" + std::to_string(i), "v");
    p3.feed(body + kEnd);
    EXPECT_TRUE(p3.failed());
    EXPECT_LE(seen, 3);
}

TEST(MultipartTest, IncompleteUploadIsDeletedNotReported) {
    auto dir = scratch_dir("incomplete");
    MultipartLimits limits;
    limits.upload_dir = dir.string();
    bool reported = false;
    {
        MultipartStreamParser parser("b", nullptr,
            [&](const std::string&, const std::string&, const std::string&, const std::string&) { reported = true; }, limits);
        parser.feed("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x.bin\"\r\n\r\npartial data");
        EXPECT_EQ(files_in(dir), 1u);
    } // client disconnected: parser destroyed mid-part
    EXPECT_FALSE(reported);
    EXPECT_EQ(files_in(dir), 0u);

    MultipartStreamParser ended("b", nullptr,
        [&](const std::string&, const std::string&, const std::string&, const std::string&) { reported = true; }, limits);
    ended.feed("--b\r\nContent-Disposition: form-data; name=\"f\"; filename=\"x.bin\"\r\n\r\ntruncated");
    ended.end();
    EXPECT_FALSE(reported);
    EXPECT_EQ(files_in(dir), 0u);
    std::filesystem::remove_all(dir);
}

TEST(MultipartTest, NameIsNotTakenFromFilename) {
    std::string seen_name;
    MultipartStreamParser parser("b", nullptr,
        [&](const std::string& name, const std::string&, const std::string&, const std::string& path) {
            seen_name = name;
            std::filesystem::remove(path);
        });
    parser.feed("--b\r\nContent-Disposition: form-data; filename=\"report.pdf\"; name=\"doc\"\r\n\r\nx\r\n" + kEnd);
    EXPECT_EQ(seen_name, "doc");
}

TEST(MultipartBoundaryTest, TokenQuotedAndOtherParameters) {
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=abc123"), "abc123");
    EXPECT_EQ(multipart_boundary("multipart/form-data; charset=utf-8; boundary=abc"), "abc");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=abc; charset=utf-8"), "abc");
    EXPECT_EQ(multipart_boundary("multipart/form-data; BOUNDARY=Up"), "Up");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=\"with space;and=semi\""), "with space;and=semi");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=\"esc\\\"aped\""), "esc\"aped");
}

TEST(MultipartBoundaryTest, MissingOrInvalid) {
    EXPECT_EQ(multipart_boundary("multipart/form-data"), "");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary="), "");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=\"unterminated"), "");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=" + std::string(71, 'b')), "");
    EXPECT_EQ(multipart_boundary("multipart/form-data; boundary=" + std::string(70, 'b')), std::string(70, 'b'));
    EXPECT_EQ(multipart_boundary("multipart/form-data; xboundary=abc"), "");
}

TEST(MultipartBoundaryTest, MultipartFormUsesTheParsedBoundary) {
    // The old parser took everything after "boundary=", quotes included.
    std::string body = "--b1\r\nContent-Disposition: form-data; name=\"f\"\r\n\r\nv\r\n--b1--\r\n";
    auto form = MultipartForm::parse("multipart/form-data; boundary=\"b1\"; charset=utf-8", body);
    EXPECT_EQ(form.fields["f"], "v");
}

TEST(MultipartTest, CompleteOnlyAfterTheClosingBoundary) {
    std::string part = "--bnd\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n";
    {
        MultipartStreamParser parser("bnd", [](const std::string&, const std::string&) {},
                                     [](const std::string&, const std::string&, const std::string&, const std::string&) {});
        parser.feed(part);
        parser.end();
        EXPECT_FALSE(parser.complete()); // truncated: no "--bnd--"
    }
    {
        MultipartStreamParser parser("bnd", [](const std::string&, const std::string&) {},
                                     [](const std::string&, const std::string&, const std::string&, const std::string&) {});
        parser.feed(part + "--bnd--\r\n");
        parser.end();
        EXPECT_TRUE(parser.complete());
    }
}
