// libFuzzer harness for orbit::http::Http1Parser (llhttp). The first byte picks the
// piece size, so the same request is also fed split at different points; the
// parser is resumed through every pause as a connection would drive it.

#include <orbit/http/Http1Parser.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    const size_t piece = 1 + data[0] % 64;
    std::string_view input(reinterpret_cast<const char*>(data) + 1, size - 1);

    orbit::http::Http1Parser::Limits limits;
    limits.max_body_size = 64 * 1024;
    orbit::http::Http1Parser parser(limits);

    // Each byte yields at most a couple of pauses; more means no progress.
    size_t budget = 4 * size + 16;
    while (!input.empty()) {
        std::string_view chunk = input.substr(0, piece);
        while (!chunk.empty()) {
            if (budget-- == 0) std::abort(); // the parser stopped making progress
            size_t used = 0;
            auto event = parser.feed(chunk, used);
            if (used > chunk.size()) std::abort(); // never claims more than it was given
            chunk.remove_prefix(used);
            input.remove_prefix(used);
            if (event == orbit::http::Http1Parser::Event::Error) return 0;
            if (event == orbit::http::Http1Parser::Event::NeedMore) break;
            if (event == orbit::http::Http1Parser::Event::MessageComplete) {
                volatile size_t touch = parser.request().uri.size() + parser.request().body.size();
                (void)touch;
                if (parser.upgrade()) return 0; // the rest is another protocol
                parser.next();
            }
        }
    }
    return 0;
}
