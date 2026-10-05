#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <functional>

namespace http {

/**
 * @brief Limits for MultipartStreamParser. Exceeding one fails the parse
 *        (see MultipartStreamParser::failed()) and deletes the partial file.
 */
struct MultipartLimits {
    size_t max_file_size = 100 * 1024 * 1024;        ///< Per uploaded file
    size_t max_field_size = 1024 * 1024;             ///< Per non-file field (kept in memory)
    size_t max_total_size = 1024ull * 1024 * 1024;   ///< Whole body
    size_t max_parts = 1000;                         ///< Fields plus files
    /// Where files are written. Empty: a private directory (mode 0700 on
    /// POSIX) created once per process under the system temp directory.
    std::string upload_dir;
};

/**
 * @brief Extracts the boundary parameter from a multipart Content-Type value,
 *        e.g. `multipart/form-data; boundary="abc"` (RFC 9110 section 5.6.6,
 *        RFC 2046 section 5.1.1).
 * @return The boundary, unquoted; empty if absent or invalid (more than 70
 *         characters, or an unterminated quoted string).
 */
std::string multipart_boundary(std::string_view content_type);

/**
 * @brief Streaming parser for multipart/form-data.
 *
 * Files are written to unpredictable names, created exclusively with
 * owner-only permissions. A file passed to the OnFileCallback belongs to the
 * application, which should move or delete it; files of parts that did not
 * complete (malformed input, a limit, or the parser being destroyed) are
 * deleted by the parser and never reported.
 */
class MultipartStreamParser {
public:
    using OnFieldCallback = std::function<void(const std::string& name, const std::string& value)>;
    /// `filename` is chosen by the client: never use it as a path.
    using OnFileCallback = std::function<void(const std::string& name, const std::string& filename, const std::string& content_type, const std::string& tmp_filepath)>;

    /**
     * @brief Constructs a new MultipartStreamParser.
     * @param boundary The boundary string from the Content-Type header.
     * @param on_field Callback invoked when a standard form field is parsed.
     * @param on_file Callback invoked when a file is parsed and saved to a temporary location.
     * @param limits Size limits and upload directory.
     */
    MultipartStreamParser(const std::string& boundary, OnFieldCallback on_field, OnFileCallback on_file,
                          MultipartLimits limits = {});
    ~MultipartStreamParser();

    MultipartStreamParser(const MultipartStreamParser&) = delete;
    MultipartStreamParser& operator=(const MultipartStreamParser&) = delete;

    /**
     * @brief Feeds a chunk of data into the parser.
     * @param chunk The data chunk to parse.
     * @return false once the parse has failed; later chunks are ignored.
     */
    bool feed(std::string_view chunk);

    /**
     * @brief Signals the end of the input stream.
     */
    void end();

    /// True if a limit was exceeded or a file could not be written.
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }
    /// True once the closing boundary has been seen without a failure. A
    /// body that ended before it (a truncated upload) is not complete.
    bool complete() const { return finished_ && !failed_; }

private:
    std::string boundary_;
    std::string dash_boundary_;
    OnFieldCallback on_field_;
    OnFileCallback on_file_;
    MultipartLimits limits_;

    enum class State {
        FINDING_BOUNDARY,
        READING_HEADERS,
        READING_BODY
    };

    State state_ = State::FINDING_BOUNDARY;
    std::string buffer_;

    std::string current_name_;
    std::string current_filename_;
    std::string current_content_type_;
    std::string current_field_value_;
    int current_fd_ = -1;
    size_t current_size_ = 0;
    std::string current_tmp_filepath_;
    size_t total_size_ = 0;
    size_t parts_ = 0;
    bool failed_ = false;
    bool finished_ = false;
    std::string error_;

    void process_buffer();
    void parse_headers(std::string_view headers);
    bool open_temp_file();
    bool append_body(std::string_view data);
    void close_current_part();
    void discard_current_part();
    void fail(const std::string& reason);
};

} // namespace http
