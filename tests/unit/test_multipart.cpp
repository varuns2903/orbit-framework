#include <gtest/gtest.h>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>

using namespace http;

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
        EXPECT_NE(data.find("/tmp/orbit_uploads"), std::string::npos);
        
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
