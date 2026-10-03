#pragma once
#include <string>
#include <functional>
#include <vector>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string_view>

#include <zlib.h>

namespace server {
    class Connection; // Forward declaration
}

namespace http {
namespace websocket {

namespace detail {

/**
 * @brief A decoded RFC 6455 frame header.
 */
struct FrameHeader {
    bool fin;
    uint8_t opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask_key[4];
    size_t header_length;
};

/**
 * @brief Decodes an RFC 6455 frame header from the front of a buffer.
 *
 * Returns false when the buffer does not yet hold the complete header *and*
 * payload, so the caller can wait for more data. On success, header_length is
 * the number of bytes preceding the payload, including the extended length and
 * masking key when present.
 *
 * @param buffer The raw bytes received so far.
 * @param header Populated on success.
 * @return True if a complete frame is available at the front of the buffer.
 */
bool parse_frame_header(const std::vector<char>& buffer, FrameHeader& header);

/**
 * @brief Progress of parsing the frame at the front of a buffer.
 */
enum class FrameStatus {
    NeedHeader,   ///< The frame header has not fully arrived.
    NeedPayload,  ///< The header is parsed (payload_length is known); the payload has not fully arrived.
    Complete,     ///< The whole frame is in the buffer.
    Invalid       ///< The frame violates RFC 6455 and the connection must be failed.
};

/**
 * @brief Parses the frame header at the front of @p buffer.
 *
 * Unlike parse_frame_header(), this distinguishes a protocol violation from
 * a frame that is still arriving, and reports the payload length as soon as
 * the header is available so callers can reject oversized frames without
 * buffering them.
 */
FrameStatus inspect_frame(const std::vector<char>& buffer, FrameHeader& header);

/**
 * @brief Unmasks a frame payload in place using the frame's masking key.
 * @param payload The masked payload bytes.
 * @param mask_key The 4-byte masking key from the frame header.
 */
void unmask_payload(std::string& payload, const uint8_t mask_key[4]);

/// True if data is well-formed UTF-8 (no overlong forms, surrogates, or code
/// points above U+10FFFF), as RFC 6455 requires of text messages and close reasons.
bool is_valid_utf8(std::string_view data);

/// True if a close frame may carry this status code (RFC 6455 section 7.4).
bool is_valid_close_code(uint16_t code);

} // namespace detail

/**
 * @brief Represents an active WebSocket connection.
 */
class WebSocketConnection {
public:
    /// Where frames go. Lets the protocol logic run without a socket (tests).
    struct Transport {
        std::function<void(const std::vector<char>&)> write; ///< Queue bytes to the peer.
        std::function<void()> close;                          ///< Close once queued bytes are sent.
    };

    explicit WebSocketConnection(server::Connection& underlying_connection, bool enable_deflate = false);
    explicit WebSocketConnection(Transport transport, bool enable_deflate = false);
    ~WebSocketConnection();

    // User-facing API

    /**
     * @brief Registers a callback to be called when a message is received.
     * @param handler The callback function.
     */
    void on_message(std::function<void(const std::string&)> handler);

    /**
     * @brief Registers a callback for binary messages. When set, binary
     *        messages go here and on_message() receives only text; when not
     *        set, on_message() receives both, as before.
     */
    void on_binary_message(std::function<void(const std::string&)> handler);

    /**
     * @brief Registers a callback to be called when the connection is closed.
     * @param handler The callback function.
     */
    void on_close(std::function<void()> handler);
    
    /**
     * @brief Sends a text message over the WebSocket connection.
     *        Safe to call from any thread.
     * @param message The message to send.
     */
    void send(const std::string& message);

    /**
     * @brief Sends a binary message. Safe to call from any thread.
     */
    void send_binary(const std::string& data);

    /**
     * @brief Closes the WebSocket connection gracefully.
     */
    void close();

    /**
     * @brief Internal: the underlying transport has gone away (peer disconnect,
     *        timeout or I/O error). Fires the close handler once, if it has not
     *        already run, and makes further send() calls no-ops.
     */
    void handle_transport_closed();

    /**
     * @brief Sets the largest message payload accepted from the client, after
     *        decompression. Larger messages close the connection with 1009.
     * @param bytes Limit in bytes (default 16 MiB).
     */
    void set_max_message_size(size_t bytes) { max_message_size_ = bytes; }

    // Internal API called by Connection::handle_read when in WEBSOCKET state
    void process_raw_data(std::vector<char>& buffer);

private:
    Transport transport_;
    std::function<void(const std::string&)> message_handler_;
    std::function<void(const std::string&)> binary_handler_;
    std::function<void()> close_handler_;
    
    std::atomic<bool> is_closed_{false};
    size_t max_message_size_{16 * 1024 * 1024};

    // Serialises frame construction, compression and queueing: send() may be
    // called from any thread (e.g. EventRouter broadcasts) while the event
    // loop answers pings and closes.
    std::mutex send_mutex_;

    // Message being reassembled from fragments (event-loop thread only).
    uint8_t fragment_opcode_{0}; // 0 = none in progress
    bool fragment_compressed_{false};
    std::string fragment_payload_;

    void send_frame(uint8_t opcode, const std::string& payload);
    void write_frame_locked(uint8_t first_byte, const std::string& payload);
    void fail_connection(uint16_t status_code);
    // Returns false if the connection was failed.
    bool deliver_message(uint8_t opcode, bool compressed, std::string payload);
    void handle_close_frame(const std::string& payload);

    bool deflate_enabled_{false};
    z_stream inflate_stream_{};
    z_stream deflate_stream_{};
    bool streams_initialized_{false};
    
    void init_streams();
    void cleanup_streams();
    std::string deflate_payload(const std::string& payload);
    bool inflate_payload(const std::string& payload, std::string& out);
};

} // namespace websocket
} // namespace http
