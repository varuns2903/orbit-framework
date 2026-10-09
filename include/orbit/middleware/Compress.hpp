#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/routing/Router.hpp>
#include <string_view>
#include <vector>

namespace orbit::middleware {

/**
 * @defgroup middlewares Middlewares
 * @brief Built-in middleware components for Orbit HTTP server.
 */

/// A response content coding (RFC 9110 section 8.4.1).
enum class ContentCoding {
    Identity, ///< No compression.
    Gzip,     ///< "gzip" (zlib, always available).
    Brotli,   ///< "br" (needs ORBIT_ENABLE_BROTLI).
    Zstd      ///< "zstd" (needs ORBIT_ENABLE_ZSTD, RFC 8878).
};

/// Options for compress().
struct CompressOptions {
    /// Codings offered, best first. Codings this build lacks are skipped.
    std::vector<ContentCoding> preference{ContentCoding::Brotli, ContentCoding::Zstd, ContentCoding::Gzip};
    /// Bodies smaller than this are sent as they are.
    size_t min_size = 150;
    int gzip_level = 6;     ///< 1 (fast) to 9 (small)
    int brotli_quality = 5; ///< 0 to 11; 4 to 6 suits responses made per request
    int zstd_level = 3;     ///< 1 to 19
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that compresses response bodies with brotli,
 *        zstd or gzip, whichever the client accepts and the server prefers.
 *
 * The coding is negotiated from Accept-Encoding (see negotiate_coding()).
 * Every compressible response gets "Vary: Accept-Encoding", so shared
 * caches keep the variants apart. Responses that already have a
 * Content-Encoding, ask for Cache-Control: no-transform, are small, are
 * already-compressed media, or are streamed are left alone. A strong ETag on
 * a compressed body is made weak, since the bytes differ from the
 * uncompressed variant.
 *
 * @return routing::Middleware The compression middleware handler.
 */
routing::Middleware compress(CompressOptions options = {});

/**
 * @brief Chooses the coding for a response.
 *
 * The acceptable coding with the highest q-value wins; ties go to the first
 * in @p preference. An explicit entry overrides "*", "x-gzip" counts as
 * "gzip", and q=0 refuses a coding (RFC 9110 section 12.5.3). Codings not
 * available in this build are never chosen, unless @p only_available is
 * false (the body is already compressed, e.g. a precompressed file).
 *
 * @return The coding to use, or Identity if none is acceptable.
 */
ContentCoding negotiate_coding(std::string_view accept_encoding, const std::vector<ContentCoding>& preference,
                               bool only_available = true);

/// True if this build can produce @p coding.
bool coding_available(ContentCoding coding);

/// The Content-Encoding token for @p coding ("gzip", "br", "zstd"; "" for identity).
std::string_view coding_name(ContentCoding coding);

/**
 * @brief Compresses @p in with @p coding at the levels in @p options.
 * @return False if the coding is unavailable or compression failed.
 */
bool compress_body(ContentCoding coding, const std::string& in, std::string& out, const CompressOptions& options = {});

/**
 * @brief Whether an Accept-Encoding header value allows gzip.
 */
bool accepts_gzip(std::string_view accept_encoding);

} // namespace middleware
