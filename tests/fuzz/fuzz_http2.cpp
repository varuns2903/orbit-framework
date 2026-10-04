// Fuzzes the HTTP/2 server session: frame parsing (nghttp2), HPACK header
// decoding and Orbit's header handling (on_header / pseudo-headers), body
// accumulation and stream lifecycle.
//
// Every input starts after the client connection preface, so the fuzzer
// spends its time on frames rather than rediscovering the magic string.
#include <orbit/http/Http2Session.hpp>
#include <orbit/network/EpollProactor.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static network::EpollProactor proactor;
    static routing::Router router; // no routes: every request ends in a 404
    static concurrency::ThreadPool pool(1);

    // No connection: whatever the session writes back is dropped.
    auto session = std::make_shared<http::h2::Http2Session>(std::weak_ptr<server::Connection>(), proactor, router,
                                                            pool, "127.0.0.1", 64 * 1024);
    static const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    std::string input = preface;
    input.append(reinterpret_cast<const char*>(data), size);
    session->process_data(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    return 0;
}
