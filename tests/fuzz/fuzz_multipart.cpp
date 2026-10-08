// Fuzzes multipart/form-data parsing, both the in-memory parser and the
// streaming parser used for uploads.
//
// The input's first line is the boundary; the rest is the body. Byte 0 also
// picks the chunk size for the streaming parser.
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    std::string_view input(reinterpret_cast<const char*>(data) + 1, size - 1);
    const size_t chunk = static_cast<size_t>(data[0] % 128) + 1;

    size_t newline = input.find('\n');
    if (newline == std::string_view::npos || newline == 0 || newline > 70) return 0; // RFC 2046 boundary length
    std::string boundary(input.substr(0, newline));
    std::string_view body = input.substr(newline + 1);

    (void)orbit::http::MultipartForm::parse("multipart/form-data; boundary=" + boundary, body);

    orbit::http::MultipartStreamParser parser(
        boundary, [](const std::string&, const std::string&) {},
        [](const std::string&, const std::string&, const std::string&, const std::string& path) {
            std::error_code ec;
            std::filesystem::remove(path, ec); // keep the fuzzer from filling the disk
        });
    for (size_t i = 0; i < body.size(); i += chunk) {
        parser.feed(body.substr(i, std::min(chunk, body.size() - i)));
    }
    return 0;
}
