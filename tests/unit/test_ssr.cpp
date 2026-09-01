#include <gtest/gtest.h>
#include <orbit/http/HttpResponse.hpp>
#include <fstream>
#include <filesystem>

TEST(SsrTest, RendersTemplateCorrectly) {
    // Create a temporary template file
    std::string temp_dir = std::filesystem::temp_directory_path().string();
    std::string template_path = temp_dir + "/template.html";
    std::ofstream out(template_path);
    out << "<h1>Hello {{ name }}!</h1>";
    out.close();

    http::HttpResponse res;
    nlohmann::json data;
    data["name"] = "World";

    res.render(template_path, data);

    EXPECT_EQ(res.status_code, http::HttpStatus::OK);
    EXPECT_EQ(res.headers["Content-Type"], "text/html");
    EXPECT_EQ(res.body, "<h1>Hello World!</h1>");
    
    std::filesystem::remove(template_path);
}

TEST(SsrTest, HandlesTemplateError) {
    http::HttpResponse res;
    nlohmann::json data;
    
    // Non-existent template
    res.render("/tmp/nonexistent_template_xyz123.html", data);

    EXPECT_EQ(res.status_code, http::HttpStatus::InternalServerError);
    EXPECT_EQ(res.headers["Content-Type"], "text/html");
    EXPECT_TRUE(res.body.find("500 Internal Server Error") != std::string::npos);
}
