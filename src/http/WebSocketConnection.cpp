#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/server/Connection.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <cstring>

namespace http {
namespace websocket {

namespace {

constexpr uint8_t kContinuation = 0x0;
constexpr uint8_t kText = 0x1;
constexpr uint8_t kBinary = 0x2;
constexpr uint8_t kClose = 0x8;
constexpr uint8_t kPing = 0x9;
constexpr uint8_t kPong = 0xA;

} // namespace

WebSocketConnection::WebSocketConnection(server::Connection& underlying_connection, bool enable_deflate)
    : WebSocketConnection(Transport{
          [&underlying_connection](const std::vector<char>& data) { underlying_connection.write_raw(data); },
          [&underlying_connection]() { underlying_connection.mark_for_close(); }},
      enable_deflate) {}

WebSocketConnection::WebSocketConnection(Transport transport, bool enable_deflate)
    : transport_(std::move(transport)), deflate_enabled_(enable_deflate) {
    if (deflate_enabled_) {
        init_streams();
    }
}

WebSocketConnection::~WebSocketConnection() {
    if (streams_initialized_) {
        cleanup_streams();
    }
}

void WebSocketConnection::init_streams() {
    std::memset(&inflate_stream_, 0, sizeof(z_stream));
    std::memset(&deflate_stream_, 0, sizeof(z_stream));
    
    // -15 for raw deflate (no zlib headers)
    if (inflateInit2(&inflate_stream_, -15) != Z_OK) {
        deflate_enabled_ = false;
        return;
    }
    
    if (deflateInit2(&deflate_stream_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        inflateEnd(&inflate_stream_);
        deflate_enabled_ = false;
        return;
    }
    streams_initialized_ = true;
}

void WebSocketConnection::cleanup_streams() {
    inflateEnd(&inflate_stream_);
    deflateEnd(&deflate_stream_);
    streams_initialized_ = false;
}

std::string WebSocketConnection::deflate_payload(const std::string& payload) {

    deflate_stream_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(payload.data()));
    deflate_stream_.avail_in = static_cast<uInt>(payload.size());
    
    std::string out;
    char out_buf[16384];
    
    int ret;
    do {
        deflate_stream_.next_out = reinterpret_cast<Bytef*>(out_buf);
        deflate_stream_.avail_out = sizeof(out_buf);
        
        ret = deflate(&deflate_stream_, Z_SYNC_FLUSH);
        
        if (ret == Z_STREAM_ERROR) return payload; // Fallback
        
        size_t have = sizeof(out_buf) - deflate_stream_.avail_out;
        out.append(out_buf, have);
    } while (deflate_stream_.avail_out == 0);
    
    // Remove the 0x00 0x00 0xFF 0xFF trailer
    if (out.size() >= 4 && out.substr(out.size() - 4) == std::string("\x00\x00\xff\xff", 4)) {
        out.resize(out.size() - 4);
    }

    // We negotiate server_no_context_takeover: each message starts afresh.
    deflateReset(&deflate_stream_);
    
    return out;
}

bool WebSocketConnection::inflate_payload(const std::string& payload, std::string& out) {
    out.clear();
    // We negotiate client_no_context_takeover: each message starts afresh.
    inflateReset(&inflate_stream_);
    
    // Append the stripped trailer
    std::string in = payload + std::string("\x00\x00\xff\xff", 4);
    
    inflate_stream_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    inflate_stream_.avail_in = static_cast<uInt>(in.size());
    
    char out_buf[16384];
    
    int ret;
    do {
        inflate_stream_.next_out = reinterpret_cast<Bytef*>(out_buf);
        inflate_stream_.avail_out = sizeof(out_buf);
        
        ret = inflate(&inflate_stream_, Z_SYNC_FLUSH);
        
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR || ret == Z_NEED_DICT) return false;
        
        size_t have = sizeof(out_buf) - inflate_stream_.avail_out;
        // Bound the output so a small compressed frame cannot expand without limit.
        if (out.size() + have > max_message_size_) return false;
        out.append(out_buf, have);
    } while (inflate_stream_.avail_out == 0);
    
    return true;
}

void WebSocketConnection::on_message(std::function<void(const std::string&)> handler) {
    message_handler_ = std::move(handler);
}

void WebSocketConnection::on_binary_message(std::function<void(const std::string&)> handler) {
    binary_handler_ = std::move(handler);
}

void WebSocketConnection::on_close(std::function<void()> handler) {
    close_handler_ = std::move(handler);
}

// Builds one unmasked frame (servers never mask) and queues it. Caller
// holds send_mutex_, so frames from different threads never interleave.
void WebSocketConnection::write_frame_locked(uint8_t first_byte, const std::string& payload) {
    std::vector<char> frame;
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(first_byte));

    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), payload.begin(), payload.end());
    transport_.write(frame);
}

void WebSocketConnection::send_frame(uint8_t opcode, const std::string& payload) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (is_closed_) return;

    uint8_t byte0 = static_cast<uint8_t>(0x80 | opcode); // FIN
    if (deflate_enabled_ && (opcode == kText || opcode == kBinary)) {
        byte0 |= 0x40; // RSV1: compressed
        write_frame_locked(byte0, deflate_payload(payload));
    } else {
        write_frame_locked(byte0, payload);
    }
}

void WebSocketConnection::send(const std::string& message) {
    send_frame(kText, message);
}

void WebSocketConnection::send_binary(const std::string& data) {
    send_frame(kBinary, data);
}

void WebSocketConnection::close() {
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (is_closed_.exchange(true)) return;
        // Status 1000: normal closure.
        write_frame_locked(0x80 | kClose, std::string("\x03\xe8", 2));
        transport_.close();
    }
    if (close_handler_) {
        close_handler_();
    }
}

void WebSocketConnection::handle_transport_closed() {
    if (is_closed_.exchange(true)) return;
    if (close_handler_) {
        close_handler_();
    }
}

void WebSocketConnection::fail_connection(uint16_t status_code) {
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (is_closed_.exchange(true)) return;
        // Close frame carrying a status code (RFC 6455 section 5.5.1).
        std::string code;
        code.push_back(static_cast<char>((status_code >> 8) & 0xFF));
        code.push_back(static_cast<char>(status_code & 0xFF));
        write_frame_locked(0x80 | kClose, code);
        transport_.close();
    }

    if (close_handler_) {
        close_handler_();
    }
}

void WebSocketConnection::handle_close_frame(const std::string& payload) {
    // Section 5.5.1: an optional 2-byte status code, then an optional UTF-8 reason.
    if (payload.size() == 1) {
        fail_connection(1002);
        return;
    }
    std::string reply;
    if (payload.size() >= 2) {
        uint16_t code = static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]));
        if (!detail::is_valid_close_code(code)) {
            fail_connection(1002);
            return;
        }
        if (!detail::is_valid_utf8(std::string_view(payload).substr(2))) {
            fail_connection(1007);
            return;
        }
        reply = payload.substr(0, 2); // echo the status code (section 5.5.1)
    }

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        // If we already sent a close, this is the peer's answer: just finish.
        if (!is_closed_.exchange(true)) {
            write_frame_locked(0x80 | kClose, reply);
        }
        transport_.close();
    }
    if (close_handler_) close_handler_();
}

bool WebSocketConnection::deliver_message(uint8_t opcode, bool compressed, std::string payload) {
    if (compressed) {
        std::string inflated;
        if (!inflate_payload(payload, inflated)) {
            fail_connection(1009);
            return false;
        }
        payload = std::move(inflated);
    }
    if (opcode == kText && !detail::is_valid_utf8(payload)) {
        fail_connection(1007); // Invalid frame payload data
        return false;
    }
    if (opcode == kBinary && binary_handler_) {
        binary_handler_(payload);
    } else if (message_handler_) {
        message_handler_(payload);
    }
    return !is_closed_;
}

namespace detail {

bool is_valid_utf8(std::string_view data) {
    size_t i = 0;
    const size_t n = data.size();
    while (i < n) {
        uint8_t c = static_cast<uint8_t>(data[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false; // continuation byte, overlong lead (C0/C1) or > U+10FFFF
        }
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            uint8_t cc = static_cast<uint8_t>(data[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false; // overlong
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;       // range, surrogates
        i += len;
    }
    return true;
}

bool is_valid_close_code(uint16_t code) {
    if (code >= 3000 && code <= 4999) return true; // registered / private use
    switch (code) {
        case 1000: case 1001: case 1002: case 1003:
        case 1007: case 1008: case 1009: case 1010: case 1011:
            return true;
        default:
            return false; // includes 1004-1006 and 1015, which must not be sent
    }
}

void unmask_payload(std::string& payload, const uint8_t mask_key[4]) {
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask_key[i % 4]);
    }
}

FrameStatus inspect_frame(const std::vector<char>& buffer, FrameHeader& header) {
    if (buffer.size() < 2) return FrameStatus::NeedHeader;

    uint8_t byte0 = static_cast<uint8_t>(buffer[0]);
    uint8_t byte1 = static_cast<uint8_t>(buffer[1]);

    header.fin = (byte0 & 0x80) != 0;
    header.opcode = byte0 & 0x0F;
    header.masked = (byte1 & 0x80) != 0;
    
    uint8_t initial_len = byte1 & 0x7F;
    header.header_length = 2;
    header.payload_length = initial_len;

    if (initial_len == 126) {
        if (buffer.size() < 4) return FrameStatus::NeedHeader;
        header.payload_length = (static_cast<uint64_t>(static_cast<uint8_t>(buffer[2])) << 8) |
                                 static_cast<uint64_t>(static_cast<uint8_t>(buffer[3]));
        header.header_length += 2;
    } else if (initial_len == 127) {
        if (buffer.size() < 10) return FrameStatus::NeedHeader;
        uint64_t host_len = 0;
        for (size_t i = 0; i < 8; ++i) {
            host_len = (host_len << 8) | static_cast<uint64_t>(static_cast<uint8_t>(buffer[2 + i]));
        }
        // RFC 6455 section 5.2: the most significant bit must be 0.
        if (host_len & (uint64_t{1} << 63)) return FrameStatus::Invalid;
        header.payload_length = host_len;
        header.header_length += 8;
    }

    // Control frames carry at most 125 bytes and cannot be fragmented (section 5.5).
    if ((header.opcode & 0x08) && (header.payload_length > 125 || !header.fin)) {
        return FrameStatus::Invalid;
    }

    if (header.masked) {
        if (buffer.size() < header.header_length + 4) return FrameStatus::NeedHeader;
        std::memcpy(header.mask_key, buffer.data() + header.header_length, 4);
        header.header_length += 4;
    }

    // Written so that a huge payload_length cannot wrap the sum.
    if (buffer.size() - header.header_length < header.payload_length) return FrameStatus::NeedPayload;

    return FrameStatus::Complete;
}

bool parse_frame_header(const std::vector<char>& buffer, FrameHeader& header) {
    return inspect_frame(buffer, header) == FrameStatus::Complete;
}

} // namespace detail

void WebSocketConnection::process_raw_data(std::vector<char>& buffer) {
    while (!buffer.empty() && !is_closed_) {
        detail::FrameHeader header;
        detail::FrameStatus status = detail::inspect_frame(buffer, header);
        if (status == detail::FrameStatus::Invalid) {
            buffer.clear();
            fail_connection(1002); // Protocol error
            return;
        }
        if (status == detail::FrameStatus::NeedHeader) {
            break;
        }

        const uint8_t byte0 = static_cast<uint8_t>(buffer[0]);
        const bool rsv1 = (byte0 & 0x40) != 0;
        const bool is_control = (header.opcode & 0x08) != 0;
        const bool is_data = header.opcode == kText || header.opcode == kBinary;

        // Checks that need only the header, so a bad frame is refused before
        // its payload is buffered (section 5.2).
        bool protocol_error =
            !header.masked ||                                    // clients must mask (5.1)
            (byte0 & 0x30) != 0 ||                               // RSV2/RSV3: no extension defines them
            (rsv1 && !(deflate_enabled_ && is_data)) ||          // RSV1 only on the first frame of a compressed message
            (!is_data && !is_control && header.opcode != kContinuation) || // reserved non-control opcodes
            (is_control && header.opcode != kClose && header.opcode != kPing && header.opcode != kPong) ||
            (header.opcode == kContinuation && fragment_opcode_ == 0) ||  // nothing to continue
            (is_data && fragment_opcode_ != 0);                           // new message mid-fragment
        if (protocol_error) {
            buffer.clear();
            fail_connection(1002);
            return;
        }

        // Reject oversized frames from the header alone, before buffering the payload.
        const size_t already = header.opcode == kContinuation ? fragment_payload_.size() : 0;
        if (header.payload_length > max_message_size_ || already + header.payload_length > max_message_size_) {
            buffer.clear();
            fail_connection(1009); // Message too big
            return;
        }
        if (status == detail::FrameStatus::NeedPayload) {
            break;
        }

        std::string payload(buffer.data() + header.header_length, static_cast<size_t>(header.payload_length));
        detail::unmask_payload(payload, header.mask_key);
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(header.header_length + header.payload_length));

        if (header.opcode == kClose) {
            handle_close_frame(payload);
            buffer.clear();
            return;
        }
        if (header.opcode == kPing) {
            // A pong carries the ping's payload (section 5.5.3).
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!is_closed_) write_frame_locked(0x80 | kPong, payload);
            continue;
        }
        if (header.opcode == kPong) {
            continue; // unsolicited pongs are allowed and ignored
        }

        if (is_data) {
            if (header.fin) {
                if (!deliver_message(header.opcode, rsv1, std::move(payload))) {
                    buffer.clear();
                    return;
                }
            } else {
                fragment_opcode_ = header.opcode;
                fragment_compressed_ = rsv1;
                fragment_payload_ = std::move(payload);
            }
            continue;
        }

        // Continuation frame.
        fragment_payload_ += payload;
        if (header.fin) {
            uint8_t opcode = fragment_opcode_;
            bool compressed = fragment_compressed_;
            std::string message = std::move(fragment_payload_);
            fragment_opcode_ = 0;
            fragment_compressed_ = false;
            fragment_payload_.clear();
            if (!deliver_message(opcode, compressed, std::move(message))) {
                buffer.clear();
                return;
            }
        }
    }
}

} // namespace websocket
} // namespace http
