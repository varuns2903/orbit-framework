#include <orbit/middleware/Compress.hpp>
#include <zlib.h>
#ifdef ORBIT_ENABLE_BROTLI
#include <brotli/encode.h>
#endif
#ifdef ORBIT_ENABLE_ZSTD
#include <zstd.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace orbit::middleware {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

// q-value of one Accept-Encoding element ("gzip;q=0.5"); 1 when absent,
// 0 when malformed (RFC 9110 section 12.4.2).
double quality(std::string_view params) {
    while (!params.empty()) {
        size_t semi = params.find(';');
        std::string_view param = trim(params.substr(0, semi));
        if (param.size() > 2 && (param[0] == 'q' || param[0] == 'Q') && param[1] == '=') {
            std::string value(param.substr(2));
            char* end = nullptr;
            double q = std::strtod(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || q < 0 || q > 1) return 0;
            return q;
        }
        if (semi == std::string_view::npos) break;
        params.remove_prefix(semi + 1);
    }
    return 1;
}

bool is_compressible_type(std::string_view content_type) {
    if (icontains(content_type, "image/svg")) return true;
    return !(icontains(content_type, "image/") || icontains(content_type, "video/") ||
             icontains(content_type, "audio/") || icontains(content_type, "application/zip") ||
             icontains(content_type, "application/gzip") || icontains(content_type, "application/zstd") ||
             icontains(content_type, "font/woff"));
}

void add_vary_accept_encoding(http::HttpResponse& res) {
    auto it = res.headers.find("Vary");
    if (it == res.headers.end() || trim(it->second).empty()) {
        res.headers["Vary"] = "Accept-Encoding";
    } else if (!icontains(it->second, "accept-encoding") && trim(it->second) != "*") {
        it->second += ", Accept-Encoding";
    }
}

bool gzip(const std::string& in, std::string& out, int level) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // 15 + 16 enables gzip envelope
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());

    int ret;
    char outbuffer[32768];
    out.clear();
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);
        ret = deflate(&zs, Z_FINISH);
        out.append(outbuffer, sizeof(outbuffer) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END;
}

// The q-values a client gave each coding: -1 where it said nothing.
struct Acceptance {
    double gzip = -1, br = -1, zstd = -1, star = -1;
};

Acceptance parse_accept_encoding(std::string_view accept_encoding) {
    Acceptance a;
    while (!accept_encoding.empty()) {
        size_t comma = accept_encoding.find(',');
        std::string_view element = trim(accept_encoding.substr(0, comma));
        size_t semi = element.find(';');
        std::string_view coding = trim(element.substr(0, semi));
        double q = semi == std::string_view::npos ? 1.0 : quality(element.substr(semi + 1));
        if (iequals(coding, "gzip") || iequals(coding, "x-gzip")) {
            a.gzip = std::max(a.gzip, q);
        } else if (iequals(coding, "br")) {
            a.br = std::max(a.br, q);
        } else if (iequals(coding, "zstd")) {
            a.zstd = std::max(a.zstd, q);
        } else if (coding == "*") {
            a.star = q;
        }
        if (comma == std::string_view::npos) break;
        accept_encoding.remove_prefix(comma + 1);
    }
    return a;
}

} // namespace

bool coding_available(ContentCoding coding) {
    switch (coding) {
        case ContentCoding::Identity:
        case ContentCoding::Gzip:
            return true;
        case ContentCoding::Brotli:
#ifdef ORBIT_ENABLE_BROTLI
            return true;
#else
            return false;
#endif
        case ContentCoding::Zstd:
#ifdef ORBIT_ENABLE_ZSTD
            return true;
#else
            return false;
#endif
    }
    return false;
}

std::string_view coding_name(ContentCoding coding) {
    switch (coding) {
        case ContentCoding::Gzip: return "gzip";
        case ContentCoding::Brotli: return "br";
        case ContentCoding::Zstd: return "zstd";
        case ContentCoding::Identity: break;
    }
    return "";
}

ContentCoding negotiate_coding(std::string_view accept_encoding, const std::vector<ContentCoding>& preference,
                               bool only_available) {
    Acceptance a = parse_accept_encoding(accept_encoding);
    ContentCoding best = ContentCoding::Identity;
    double best_q = 0;
    for (ContentCoding coding : preference) {
        if (coding == ContentCoding::Identity || (only_available && !coding_available(coding))) continue;
        double explicit_q = coding == ContentCoding::Gzip ? a.gzip : coding == ContentCoding::Brotli ? a.br : a.zstd;
        // An explicit entry wins over the wildcard.
        double q = explicit_q >= 0 ? explicit_q : std::max(a.star, 0.0);
        if (q > best_q) { // strictly greater: ties keep the earlier preference
            best = coding;
            best_q = q;
        }
    }
    return best;
}

bool compress_body(ContentCoding coding, const std::string& in, std::string& out, const CompressOptions& options) {
    switch (coding) {
        case ContentCoding::Gzip:
            return gzip(in, out, options.gzip_level);
        case ContentCoding::Brotli: {
#ifdef ORBIT_ENABLE_BROTLI
            size_t size = BrotliEncoderMaxCompressedSize(in.size());
            if (size == 0) return false;
            out.resize(size);
            if (!BrotliEncoderCompress(options.brotli_quality, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE, in.size(),
                                       reinterpret_cast<const uint8_t*>(in.data()), &size,
                                       reinterpret_cast<uint8_t*>(out.data()))) {
                return false;
            }
            out.resize(size);
            return true;
#else
            return false;
#endif
        }
        case ContentCoding::Zstd: {
#ifdef ORBIT_ENABLE_ZSTD
            out.resize(ZSTD_compressBound(in.size()));
            size_t size = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), options.zstd_level);
            if (ZSTD_isError(size)) return false;
            out.resize(size);
            return true;
#else
            return false;
#endif
        }
        case ContentCoding::Identity:
            break;
    }
    return false;
}

bool accepts_gzip(std::string_view accept_encoding) {
    return negotiate_coding(accept_encoding, {ContentCoding::Gzip}) == ContentCoding::Gzip;
}

routing::Middleware compress(CompressOptions options) {
    return [options](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        auto ae = request.headers.find("Accept-Encoding");
        const ContentCoding coding = ae == request.headers.end()
            ? ContentCoding::Identity
            : negotiate_coding(ae->second, options.preference);

        // Registered for every request: even an uncompressed response must
        // carry Vary, or a shared cache could hand it (or the gzip variant)
        // to the wrong client.
        writer->add_interceptor([coding, options](http::HttpResponse& res) {
            // Don't compress empty bodies or raw file descriptors
            if (res.body.empty() || res.file_fd != -1) return;
            int status = static_cast<int>(res.status_code);
            if (status < 200 || status == 204 || status == 304) return;

            // Don't compress very small payloads (overhead > savings)
            if (res.body.size() < options.min_size) return;

            // Already encoded by the handler.
            if (res.headers.count("Content-Encoding")) return;

            auto cc = res.headers.find("Cache-Control");
            if (cc != res.headers.end() && icontains(cc->second, "no-transform")) return;

            // Don't compress already compressed formats
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it != res.headers.end() && !is_compressible_type(ct_it->second)) return;

            add_vary_accept_encoding(res);
            if (coding == ContentCoding::Identity) return;

            std::string compressed_body;
            if (!compress_body(coding, res.body, compressed_body, options) ||
                compressed_body.size() >= res.body.size()) {
                return;
            }

            res.body = std::move(compressed_body);
            res.headers["Content-Encoding"] = std::string(coding_name(coding));
            res.headers["Content-Length"] = std::to_string(res.body.size());

            // The bytes differ from the identity variant, so a strong
            // validator would be wrong (RFC 9110 section 8.8.3).
            auto etag = res.headers.find("ETag");
            if (etag != res.headers.end() && etag->second.rfind("W/", 0) != 0) {
                etag->second = "W/" + etag->second;
            }
        });
        return true;
    };
}

} // namespace middleware
