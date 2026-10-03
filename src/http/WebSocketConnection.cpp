#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/server/Connection.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <cstring>
#include <iostream>

namespace http {
namespace websocket {

WebSocketConnection::WebSocketConnection(server::Connection& underlying_connection, bool enable_deflate)
    : connection_(underlying_connection), deflate_enabled_(enable_deflate) {
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
    if (payload.empty()) return "";
    
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
    
    return out;
}

bool WebSocketConnection::inflate_payload(const std::string& payload, std::string& out) {
    out.clear();
    if (payload.empty()) return true;
    
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

void WebSocketConnection::on_close(std::function<void()> handler) {
    close_handler_ = std::move(handler);
}

void WebSocketConnection::send(const std::string& message) {
    if (is_closed_) return;

    std::vector<char> frame;
    // FIN bit set, OPCODE = 1 (text)
    uint8_t byte0 = static_cast<uint8_t>(0x81);
    
    std::string final_payload = message;
    if (deflate_enabled_) {
        byte0 |= 0x40; // Set RSV1 bit
        final_payload = deflate_payload(message);
    }
    
    frame.push_back(static_cast<char>(byte0));

    size_t len = final_payload.length();
    // Server does not mask payloads
    if (len <= 125) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 65535) {
        frame.push_back(static_cast<char>(126));
        uint16_t ext_len = htons(static_cast<uint16_t>(len));
        char* ext_len_ptr = reinterpret_cast<char*>(&ext_len);
        frame.push_back(ext_len_ptr[0]);
        frame.push_back(ext_len_ptr[1]);
    } else {
        frame.push_back(static_cast<char>(127));
        // Use 64-bit length (network byte order)
        uint64_t ext_len = len; // Note: Assuming little-endian host, needs proper htobe64 in production
        // Extremely simple big-endian conversion
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((ext_len >> (i * 8)) & 0xFF));
        }
    }

    frame.insert(frame.end(), final_payload.begin(), final_payload.end());
    connection_.write_raw(frame);
}

void WebSocketConnection::close() {
    if (is_closed_.exchange(true)) return;

    // Send close frame (OPCODE 8)
    std::vector<char> frame = {static_cast<char>(0x88), 0x00};
    connection_.write_raw(frame);
    connection_.mark_for_close();
    
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
    if (is_closed_.exchange(true)) return;

    // Close frame carrying a status code (RFC 6455 section 5.5.1).
    std::vector<char> frame = {
        static_cast<char>(0x88), 0x02,
        static_cast<char>((status_code >> 8) & 0xFF), static_cast<char>(status_code & 0xFF)
    };
    connection_.write_raw(frame);
    connection_.mark_for_close();

    if (close_handler_) {
        close_handler_();
    }
}

namespace detail {

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
    while (!buffer.empty()) {
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
        // Reject oversized frames from the header alone, before buffering the payload.
        if (header.payload_length > max_message_size_) {
            buffer.clear();
            fail_connection(1009); // Message too big
            return;
        }
        if (status == detail::FrameStatus::NeedPayload) {
            break;
        }

        // We have a full frame!
        if (header.opcode == 0x8) {
            // Close frame
            if (!is_closed_.exchange(true)) {
                if (close_handler_) close_handler_();
                connection_.mark_for_close();
            }
            break;
        } else if (header.opcode == 0x1 || header.opcode == 0x2) {
            // Text or Binary frame
            std::string payload;
            payload.resize(static_cast<size_t>(header.payload_length));
            
            const char* payload_data = buffer.data() + header.header_length;
            if (header.masked) {
                payload.assign(payload_data, static_cast<size_t>(header.payload_length));
                detail::unmask_payload(payload, header.mask_key);
            } else {
                std::memcpy(&payload[0], payload_data, header.payload_length);
            }

            if (message_handler_) {
                bool rsv1 = (buffer[0] & 0x40) != 0;
                if (deflate_enabled_ && rsv1) {
                    std::string inflated;
                    if (!inflate_payload(payload, inflated)) {
                        buffer.clear();
                        fail_connection(1009);
                        return;
                    }
                    message_handler_(inflated);
                } else {
                    message_handler_(payload);
                }
            }
        } else if (header.opcode == 0x9) {
            // Ping - Send Pong (0xA)
            std::vector<char> pong_frame = {static_cast<char>(0x8A), 0x00};
            connection_.write_raw(pong_frame);
        }
        
        // Remove processed frame from buffer
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(header.header_length + header.payload_length));
    }
}

} // namespace websocket
} // namespace http
