#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/utils/Logger.hpp>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <unordered_map>

namespace middleware {

namespace {

namespace fs = std::filesystem;

struct FileInfo {
    bool regular = false;
    bool directory = false;
    uint64_t size = 0;
    std::time_t mtime = 0;
};

std::optional<FileInfo> stat_path(const fs::path& path) {
    FileInfo info;
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(path.c_str(), &st) != 0) return std::nullopt;
    info.regular = (st.st_mode & _S_IFMT) == _S_IFREG;
    info.directory = (st.st_mode & _S_IFMT) == _S_IFDIR;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
    info.regular = S_ISREG(st.st_mode);
    info.directory = S_ISDIR(st.st_mode);
#endif
    info.size = static_cast<uint64_t>(st.st_size);
    info.mtime = static_cast<std::time_t>(st.st_mtime);
    return info;
}

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// IMF-fixdate (RFC 9110 section 5.6.7), independent of the C locale.
std::string format_http_date(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[tm.tm_wday], tm.tm_mday,
                  kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9; // March-based month
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_digits(std::string_view s, int& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

// Parses an IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT"). The obsolete
// formats are not accepted; the header is then ignored, which is allowed.
std::optional<std::time_t> parse_http_date(std::string_view s) {
    if (s.size() != 29 || s.substr(3, 2) != ", " || s.substr(25) != " GMT") return std::nullopt;
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (!parse_digits(s.substr(5, 2), day) || !parse_digits(s.substr(12, 4), year) ||
        !parse_digits(s.substr(17, 2), hour) || !parse_digits(s.substr(20, 2), minute) ||
        !parse_digits(s.substr(23, 2), second)) {
        return std::nullopt;
    }
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) {
        if (s.substr(8, 3) == kMonths[i]) month = i + 1;
    }
    if (month == 0 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return std::nullopt;
    int64_t days = days_from_civil(year, month, static_cast<unsigned>(day));
    return static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string_view strip_weak(std::string_view tag) {
    return tag.substr(0, 2) == "W/" ? tag.substr(2) : tag;
}

// If-None-Match uses the weak comparison (RFC 9110 section 13.1.2).
bool etag_list_matches(std::string_view header, std::string_view etag) {
    etag = strip_weak(etag);
    while (!header.empty()) {
        size_t comma = header.find(',');
        std::string_view item = trim(header.substr(0, comma));
        if (item == "*" || strip_weak(item) == etag) return true;
        if (comma == std::string_view::npos) break;
        header.remove_prefix(comma + 1);
    }
    return false;
}

struct ByteRange {
    enum class Kind { None, Satisfiable, Unsatisfiable } kind = Kind::None;
    uint64_t first = 0;
    uint64_t last = 0; // inclusive
};

bool parse_u64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

// A single "bytes=" range. Anything else (other units, several ranges,
// malformed syntax) yields None and the whole file is sent, as RFC 9110
// section 14.2 permits.
ByteRange parse_range(std::string_view header, uint64_t size) {
    ByteRange r;
    header = trim(header);
    if (header.substr(0, 6) != "bytes=") return r;
    std::string_view spec = trim(header.substr(6));
    if (spec.find(',') != std::string_view::npos) return r;
    size_t dash = spec.find('-');
    if (dash == std::string_view::npos) return r;
    std::string_view first = trim(spec.substr(0, dash));
    std::string_view last = trim(spec.substr(dash + 1));

    if (first.empty()) {
        uint64_t suffix = 0;
        if (!parse_u64(last, suffix)) return r;
        if (suffix == 0 || size == 0) {
            r.kind = ByteRange::Kind::Unsatisfiable;
            return r;
        }
        r.first = size - std::min(suffix, size);
        r.last = size - 1;
        r.kind = ByteRange::Kind::Satisfiable;
        return r;
    }

    uint64_t a = 0;
    if (!parse_u64(first, a)) return r;
    uint64_t b = UINT64_MAX;
    if (!last.empty() && !parse_u64(last, b)) return r;
    if (b < a) return r;
    if (a >= size) {
        r.kind = ByteRange::Kind::Unsatisfiable;
        return r;
    }
    r.first = a;
    r.last = std::min(b, size - 1);
    r.kind = ByteRange::Kind::Satisfiable;
    return r;
}

enum class Resolution { Inside, Outside, Hidden };

// Canonicalises base/rel (resolving symlinks) and checks it stays inside base.
Resolution resolve(const fs::path& base, const fs::path& candidate, bool serve_dotfiles, fs::path& out) {
    std::error_code ec;
    out = fs::weakly_canonical(candidate, ec);
    if (ec) return Resolution::Hidden;
    // Component-wise, so it works with either separator and a sibling such
    // as "/srv/public-old" is not mistaken for "/srv/public".
    fs::path inside = out.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") return Resolution::Outside;
    if (!serve_dotfiles) {
        for (const auto& part : inside) {
            const auto& name = part.native();
            if (!name.empty() && name[0] == '.' && part != ".") return Resolution::Hidden;
        }
    }
    return Resolution::Inside;
}

} // namespace

std::string_view mime_type_for_extension(std::string_view extension) {
    static const std::unordered_map<std::string, std::string_view> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".map", "application/json"},
        {".webmanifest", "application/manifest+json"},
        {".txt", "text/plain; charset=utf-8"},
        {".md", "text/markdown; charset=utf-8"},
        {".csv", "text/csv; charset=utf-8"},
        {".xml", "application/xml"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".webp", "image/webp"},
        {".avif", "image/avif"},
        {".ico", "image/x-icon"},
        {".bmp", "image/bmp"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"},
        {".wasm", "application/wasm"},
        {".pdf", "application/pdf"},
        {".zip", "application/zip"},
        {".gz", "application/gzip"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".ogv", "video/ogg"},
        {".mp3", "audio/mpeg"},
        {".ogg", "audio/ogg"},
        {".wav", "audio/wav"},
    };
    std::string lower(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto it = types.find(lower);
    return it != types.end() ? it->second : std::string_view("application/octet-stream");
}

routing::Middleware static_files(const std::string& directory, StaticFilesOptions options) {
    struct State {
        std::string directory;
        StaticFilesOptions options;
        fs::path base; // canonical directory, resolved once
    };
    auto state = std::make_shared<State>();
    state->directory = directory;
    state->options = std::move(options);
    std::error_code ec;
    state->base = fs::canonical(directory, ec);
    if (ec) {
        // Created later (e.g. by a build step): resolve it per request until then.
        LOG_WARN("static_files: directory '" << directory << "' does not exist yet");
        state->base.clear();
    }

    return [state](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        if (request.method != http::HttpMethod::GET && request.method != http::HttpMethod::HEAD) {
            return true; // Continue pipeline, only GET and HEAD are served statically
        }

        try {
            fs::path base = state->base;
            if (base.empty()) {
                std::error_code dir_ec;
                base = fs::canonical(state->directory, dir_ec);
                if (dir_ec) return true;
            }
            const StaticFilesOptions& opts = state->options;

            // request.uri is already percent-decoded.
            if (request.uri.find('\0') != std::string::npos) return true;
            fs::path requested;
            switch (resolve(base, base / fs::path(request.uri).relative_path(), opts.serve_dotfiles, requested)) {
                case Resolution::Outside: {
                    LOG_WARN("Path traversal attack blocked! Attempted to access: " << request.uri);
                    http::HttpResponse res;
                    res.status(http::HttpStatus::Forbidden).send("403 Forbidden");
                    writer->send(std::move(res));
                    return false; // Handled as error, stop pipeline!
                }
                case Resolution::Hidden:
                    return true;
                case Resolution::Inside:
                    break;
            }

            auto info = stat_path(requested);
            if (!info) return true;
            if (info->directory) {
                if (opts.index.empty()) return true;
                // The index file may itself be a symlink: check it as well.
                if (resolve(base, requested / opts.index, opts.serve_dotfiles, requested) != Resolution::Inside) {
                    return true;
                }
                info = stat_path(requested);
                if (!info) return true;
            }
            if (!info->regular) return true;

            auto header = [&request](std::string_view name) -> std::optional<std::string_view> {
                auto it = request.headers.find(name);
                if (it == request.headers.end()) return std::nullopt;
                return it->second;
            };

            char etag_buf[48];
            std::snprintf(etag_buf, sizeof(etag_buf), "\"%llx-%llx\"",
                          static_cast<unsigned long long>(info->mtime),
                          static_cast<unsigned long long>(info->size));
            const std::string etag = etag_buf;
            const std::string last_modified = format_http_date(info->mtime);

            http::HttpResponse res;
            res.headers["ETag"] = etag;
            res.headers["Last-Modified"] = last_modified;
            res.headers["Cache-Control"] = opts.max_age.count() > 0
                ? "public, max-age=" + std::to_string(opts.max_age.count())
                : std::string("no-cache");
            res.headers["Accept-Ranges"] = "bytes";

            // If-None-Match takes precedence over If-Modified-Since (RFC 9110 section 13.2.2).
            bool not_modified = false;
            if (auto inm = header("If-None-Match")) {
                not_modified = etag_list_matches(*inm, etag);
            } else if (auto ims = header("If-Modified-Since")) {
                auto since = parse_http_date(*ims);
                not_modified = since && info->mtime <= *since;
            }
            if (not_modified) {
                res.status(http::HttpStatus::NotModified);
                writer->send(std::move(res));
                return false;
            }

            res.send_file(requested.string(), std::string(mime_type_for_extension(requested.extension().string())));
            if (res.file_fd == -1) return true; // vanished or unreadable

            auto range_header = header("Range");
            if (range_header && request.method == http::HttpMethod::GET) {
                // If-Range: only honour the range if the client's copy is current.
                bool current = true;
                if (auto if_range = header("If-Range")) {
                    current = *if_range == etag || *if_range == last_modified;
                }
                ByteRange range = current ? parse_range(*range_header, static_cast<uint64_t>(res.file_size))
                                          : ByteRange{};
                if (range.kind == ByteRange::Kind::Unsatisfiable) {
                    http::HttpResponse err;
                    err.status(http::HttpStatus::RangeNotSatisfiable);
                    err.headers["Content-Range"] = "bytes */" + std::to_string(res.file_size);
                    writer->send(std::move(err));
                    return false;
                }
                if (range.kind == ByteRange::Kind::Satisfiable) {
                    const auto total = res.file_size;
                    res.status(http::HttpStatus::PartialContent);
                    res.set_file_range(static_cast<off_t>(range.first), static_cast<off_t>(range.last + 1));
                    res.headers["Content-Range"] = "bytes " + std::to_string(range.first) + "-" +
                                                   std::to_string(range.last) + "/" + std::to_string(total);
                }
            }

            writer->send(std::move(res));
            return false; // File served, stop pipeline!
        } catch (const std::exception&) {
            // Unrepresentable path or filesystem error: fall through to next middleware/route
        }

        return true; // File not found, continue pipeline
    };
}

} // namespace middleware
