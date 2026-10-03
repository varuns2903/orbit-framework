#include <orbit/middleware/Compress.hpp>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace middleware {

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
             icontains(content_type, "application/gzip") || icontains(content_type, "font/woff"));
}

void add_vary_accept_encoding(http::HttpResponse& res) {
    auto it = res.headers.find("Vary");
    if (it == res.headers.end() || trim(it->second).empty()) {
        res.headers["Vary"] = "Accept-Encoding";
    } else if (!icontains(it->second, "accept-encoding") && trim(it->second) != "*") {
        it->second += ", Accept-Encoding";
    }
}

bool gzip(const std::string& in, std::string& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // 15 + 16 enables gzip envelope
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
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

} // namespace

bool accepts_gzip(std::string_view accept_encoding) {
    double gzip_q = -1; // -1: not mentioned
    double star_q = -1;
    while (!accept_encoding.empty()) {
        size_t comma = accept_encoding.find(',');
        std::string_view element = trim(accept_encoding.substr(0, comma));
        size_t semi = element.find(';');
        std::string_view coding = trim(element.substr(0, semi));
        double q = semi == std::string_view::npos ? 1.0 : quality(element.substr(semi + 1));
        if (iequals(coding, "gzip") || iequals(coding, "x-gzip")) {
            gzip_q = std::max(gzip_q, q);
        } else if (coding == "*") {
            star_q = q;
        }
        if (comma == std::string_view::npos) break;
        accept_encoding.remove_prefix(comma + 1);
    }
    // An explicit entry wins over the wildcard.
    if (gzip_q >= 0) return gzip_q > 0;
    return star_q > 0;
}

routing::Middleware compress() {
    return [](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        auto ae = request.headers.find("Accept-Encoding");
        const bool client_accepts = ae != request.headers.end() && accepts_gzip(ae->second);

        // Registered for every request: even an uncompressed response must
        // carry Vary, or a shared cache could hand it (or the gzip variant)
        // to the wrong client.
        writer->add_interceptor([client_accepts](http::HttpResponse& res) {
            // Don't compress empty bodies or raw file descriptors
            if (res.body.empty() || res.file_fd != -1) return;
            int status = static_cast<int>(res.status_code);
            if (status < 200 || status == 204 || status == 304) return;

            // Don't compress very small payloads (overhead > savings)
            if (res.body.size() < 150) return;

            // Already encoded by the handler.
            if (res.headers.count("Content-Encoding")) return;

            auto cc = res.headers.find("Cache-Control");
            if (cc != res.headers.end() && icontains(cc->second, "no-transform")) return;

            // Don't compress already compressed formats
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it != res.headers.end() && !is_compressible_type(ct_it->second)) return;

            add_vary_accept_encoding(res);
            if (!client_accepts) return;

            std::string compressed_body;
            if (!gzip(res.body, compressed_body) || compressed_body.size() >= res.body.size()) return;

            res.body = std::move(compressed_body);
            res.headers["Content-Encoding"] = "gzip";
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
