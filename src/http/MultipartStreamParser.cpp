#include <orbit/http/MultipartStreamParser.hpp>
#include <cctype>
#include <orbit/utils/Random.hpp>

#include <cerrno>
#include <filesystem>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace http {

std::string multipart_boundary(std::string_view content_type) {
    size_t pos = content_type.find(';');
    while (pos != std::string_view::npos) {
        ++pos;
        while (pos < content_type.size() && (content_type[pos] == ' ' || content_type[pos] == '\t')) ++pos;
        size_t eq = content_type.find('=', pos);
        if (eq == std::string_view::npos) return {};
        std::string_view name = content_type.substr(pos, eq - pos);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
        bool is_boundary = name.size() == 8;
        for (size_t i = 0; is_boundary && i < 8; ++i) {
            is_boundary = std::tolower(static_cast<unsigned char>(name[i])) == "boundary"[i];
        }
        std::string value;
        size_t next;
        if (eq + 1 < content_type.size() && content_type[eq + 1] == '"') {
            // quoted-string: backslash escapes the next character.
            size_t i = eq + 2;
            bool closed = false;
            for (; i < content_type.size(); ++i) {
                char ch = content_type[i];
                if (ch == '\\' && i + 1 < content_type.size()) {
                    value.push_back(content_type[++i]);
                } else if (ch == '"') {
                    closed = true;
                    ++i;
                    break;
                } else {
                    value.push_back(ch);
                }
            }
            if (!closed) return {};
            next = content_type.find(';', i);
        } else {
            next = content_type.find(';', eq + 1);
            std::string_view token = content_type.substr(eq + 1, next == std::string_view::npos ? std::string_view::npos : next - eq - 1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
            value = std::string(token);
        }
        if (is_boundary) {
            return (value.empty() || value.size() > 70) ? std::string() : value;
        }
        pos = next;
    }
    return {};
}


namespace {

// A directory only this process's user can enter, created once. Uploads
// used to go to the fixed, world-writable /tmp/orbit_uploads, where another
// local user could own the directory or plant symlinks at predictable names.
std::string default_upload_dir() {
    static std::once_flag once;
    static std::string dir;
    std::call_once(once, [] {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec) return;
        for (int attempt = 0; attempt < 16 && dir.empty(); ++attempt) {
            std::filesystem::path candidate = base / ("orbit-uploads-" + utils::secure_random_hex(8));
#ifdef _WIN32
            // The Windows temp directory is already per-user.
            if (std::filesystem::create_directory(candidate, ec) && !ec) dir = candidate.string();
#else
            // mkdir with 0700 directly: no window with wider permissions,
            // and it fails if anything (including a symlink) is already there.
            if (::mkdir(candidate.c_str(), 0700) == 0) dir = candidate.string();
#endif
        }
    });
    return dir;
}

// Creates `path` exclusively, readable and writable by the owner only.
int open_exclusive(const std::string& path) {
#ifdef _WIN32
    int fd = -1;
    if (_sopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0) {
        return -1;
    }
    return fd;
#else
    return ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
#ifdef _WIN32
        int n = _write(fd, data.data(), static_cast<unsigned int>(data.size()));
#else
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

void close_fd(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    ::close(fd);
#endif
}

// Value of a `key="..."` parameter in a Content-Disposition line. Matches
// whole parameter names, so `name` is not found inside `filename`.
std::string disposition_param(std::string_view line, std::string_view key) {
    size_t pos = 0;
    while ((pos = line.find(key, pos)) != std::string_view::npos) {
        bool starts_param = pos > 0 && (line[pos - 1] == ';' || line[pos - 1] == ' ' || line[pos - 1] == '\t');
        size_t after = pos + key.size();
        if (starts_param && line.substr(after, 2) == "=\"") {
            size_t value_start = after + 2;
            size_t value_end = line.find('"', value_start);
            if (value_end == std::string_view::npos) return "";
            return std::string(line.substr(value_start, value_end - value_start));
        }
        pos = after;
    }
    return "";
}

} // namespace

MultipartStreamParser::MultipartStreamParser(const std::string& boundary, OnFieldCallback on_field,
                                             OnFileCallback on_file, MultipartLimits limits)
    : boundary_(boundary), on_field_(std::move(on_field)), on_file_(std::move(on_file)), limits_(std::move(limits)) {
    dash_boundary_ = "--" + boundary_;
    if (limits_.upload_dir.empty()) limits_.upload_dir = default_upload_dir();
}

MultipartStreamParser::~MultipartStreamParser() {
    // A part still open here never completed: do not report a truncated file.
    discard_current_part();
}

bool MultipartStreamParser::feed(std::string_view chunk) {
    if (failed_) return false;
    total_size_ += chunk.size();
    if (total_size_ > limits_.max_total_size) {
        fail("multipart body exceeds max_total_size");
        return false;
    }
    buffer_.append(chunk);
    process_buffer();
    return !failed_;
}

void MultipartStreamParser::end() {
    if (failed_) return;
    process_buffer();
    // Input ended inside a part: it is incomplete.
    discard_current_part();
}

void MultipartStreamParser::fail(const std::string& reason) {
    if (failed_) return;
    failed_ = true;
    error_ = reason;
    discard_current_part();
    buffer_.clear();
}

void MultipartStreamParser::discard_current_part() {
    if (current_fd_ != -1) {
        close_fd(current_fd_);
        current_fd_ = -1;
    }
    if (!current_tmp_filepath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(current_tmp_filepath_, ec);
        current_tmp_filepath_.clear();
    }
    current_name_.clear();
    current_filename_.clear();
    current_content_type_.clear();
    current_field_value_.clear();
    current_size_ = 0;
}

bool MultipartStreamParser::append_body(std::string_view data) {
    current_size_ += data.size();
    if (current_fd_ != -1) {
        if (current_size_ > limits_.max_file_size) {
            fail("uploaded file exceeds max_file_size");
            return false;
        }
        if (!write_all(current_fd_, data)) {
            fail("could not write the uploaded file");
            return false;
        }
    } else {
        if (current_size_ > limits_.max_field_size) {
            fail("form field exceeds max_field_size");
            return false;
        }
        current_field_value_.append(data);
    }
    return true;
}

void MultipartStreamParser::process_buffer() {
    while (!buffer_.empty() && !failed_) {
        if (state_ == State::FINDING_BOUNDARY) {
            size_t pos = buffer_.find(dash_boundary_);
            if (pos == std::string::npos) {
                if (buffer_.length() > dash_boundary_.length()) {
                    buffer_.erase(0, buffer_.length() - dash_boundary_.length());
                }
                break;
            }
            
            buffer_.erase(0, pos + dash_boundary_.length());
            
            // Check for \r\n or --
            if (buffer_.length() >= 2) {
                if (buffer_.substr(0, 2) == "--") {
                    // The close delimiter: the body is complete.
                    finished_ = true;
                    buffer_.clear();
                    break;
                } else if (buffer_.substr(0, 2) == "\r\n") {
                    buffer_.erase(0, 2);
                    state_ = State::READING_HEADERS;
                } else {
                    // Malformed, just skip
                    buffer_.erase(0, 2);
                }
            } else {
                break; // wait for more
            }
            
        } else if (state_ == State::READING_HEADERS) {
            size_t pos = buffer_.find("\r\n\r\n");
            if (pos == std::string::npos) {
                if (buffer_.length() > 8192) {
                    fail("part headers exceed 8192 bytes");
                }
                break;
            }
            if (++parts_ > limits_.max_parts) {
                fail("too many parts");
                break;
            }
            
            parse_headers(buffer_.substr(0, pos));
            buffer_.erase(0, pos + 4);
            
            current_size_ = 0;
            if (!current_filename_.empty()) {
                if (!open_temp_file()) break;
            } else {
                current_field_value_.clear();
            }
            state_ = State::READING_BODY;
            
        } else if (state_ == State::READING_BODY) {
            std::string boundary_sig = "\r\n" + dash_boundary_;
            size_t pos = buffer_.find(boundary_sig);
            
            if (pos == std::string::npos) {
                if (buffer_.length() > boundary_sig.length()) {
                    size_t write_len = buffer_.length() - boundary_sig.length();
                    std::string_view to_write = std::string_view(buffer_).substr(0, write_len);
                    if (!append_body(to_write)) break;
                    buffer_.erase(0, write_len);
                }
                break;
            } else {
                std::string_view to_write = std::string_view(buffer_).substr(0, pos);
                if (!append_body(to_write)) break;
                
                close_current_part();
                
                buffer_.erase(0, pos + 2); // Erase \r\n
                state_ = State::FINDING_BOUNDARY;
            }
        }
    }
}

void MultipartStreamParser::parse_headers(std::string_view headers) {
    current_name_.clear();
    current_filename_.clear();
    current_content_type_.clear();
    
    size_t h_start = 0;
    while (h_start < headers.length()) {
        size_t h_end = headers.find("\r\n", h_start);
        if (h_end == std::string::npos) h_end = headers.length();
        
        std::string_view header_line = headers.substr(h_start, h_end - h_start);
        
        if (header_line.starts_with("Content-Disposition:")) {
            current_name_ = disposition_param(header_line, "name");
            current_filename_ = disposition_param(header_line, "filename");
        } else if (header_line.starts_with("Content-Type:")) {
            size_t ct_pos = header_line.find(":");
            if (ct_pos != std::string::npos) {
                current_content_type_ = header_line.substr(ct_pos + 1);
                while (!current_content_type_.empty() && current_content_type_.front() == ' ') {
                    current_content_type_.erase(0, 1);
                }
            }
        }
        h_start = h_end + 2;
    }
}

bool MultipartStreamParser::open_temp_file() {
    if (limits_.upload_dir.empty()) {
        fail("no upload directory available");
        return false;
    }
    // 128 random bits: not guessable, so nothing can be planted in advance,
    // and no shared counter for concurrent uploads to collide on.
    current_tmp_filepath_ = (std::filesystem::path(limits_.upload_dir) / ("upload-" + utils::secure_random_hex(16))).string();
    current_fd_ = open_exclusive(current_tmp_filepath_);
    if (current_fd_ == -1) {
        current_tmp_filepath_.clear(); // not ours: never delete it
        fail("could not create a temporary file for the upload");
        return false;
    }
    return true;
}

void MultipartStreamParser::close_current_part() {
    if (current_fd_ != -1) {
        close_fd(current_fd_);
        current_fd_ = -1;
        if (on_file_ && !current_name_.empty()) {
            on_file_(current_name_, current_filename_, current_content_type_, current_tmp_filepath_);
            current_tmp_filepath_.clear(); // the application owns it now
        }
    } else {
        if (on_field_ && !current_name_.empty()) {
            on_field_(current_name_, current_field_value_);
        }
    }
    // A file part without a name was never handed over: remove it.
    discard_current_part();
}

} // namespace http
