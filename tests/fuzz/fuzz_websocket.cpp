// Fuzzes the WebSocket protocol state machine: frame decoding, fragmentation,
// control frames, UTF-8 validation, close handling and permessage-deflate.
//
// Byte 0 picks options (bit 0: deflate negotiated, bit 1: tiny message
// limit); byte 1 picks the chunk size, so frames also arrive split.
#include <orbit/http/WebSocketConnection.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const bool deflate = (data[0] & 1) != 0;
    const size_t max_message = (data[0] & 2) ? 64 : (1 << 20);
    const size_t chunk = static_cast<size_t>(data[1] % 64) + 1;

    orbit::http::websocket::WebSocketConnection::Transport transport{
        [](const std::vector<char>&) {}, // drop what the server sends
        []() {}};
    orbit::http::websocket::WebSocketConnection ws(transport, deflate);
    ws.set_max_message_size(max_message);
    ws.on_message([&ws](const std::string& m) {
        if (m.size() < 32) ws.send(m); // exercise the send path (and deflate) too
    });
    ws.on_binary_message([&ws](const std::string& m) {
        if (m.size() < 32) ws.send_binary(m);
    });

    std::vector<char> buffer;
    for (size_t i = 2; i < size; i += chunk) {
        size_t end = std::min(size, i + chunk);
        buffer.insert(buffer.end(), data + i, data + end);
        ws.process_raw_data(buffer);
    }
    return 0;
}
