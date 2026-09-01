#include <orbit/http/HttpParser.hpp>
#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Convert the fuzz data into a string_view
    std::string_view raw_request(reinterpret_cast<const char*>(data), size);

    // Call the parser. We don't care about the result, only that it doesn't crash, 
    // leak memory, or trigger undefined behavior (caught by sanitizers).
    auto parsed = http::HttpParser::parse(raw_request);

    return 0; // Always return 0 to indicate the fuzzer successfully executed
}
