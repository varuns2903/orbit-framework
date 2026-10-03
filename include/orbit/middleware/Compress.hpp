#pragma once
#include <orbit/routing/Router.hpp>
#include <string_view>

namespace middleware {

/**
 * @defgroup middlewares Middlewares
 * @brief Built-in middleware components for Orbit HTTP server.
 */

/**
 * @ingroup middlewares
 * @brief Returns a middleware that automatically compresses the HTTP response body using GZIP.
 *
 * Compresses the response when the client's Accept-Encoding allows gzip
 * (q-values honoured: "gzip;q=0" refuses it, "*" accepts it). Every
 * compressible response gets "Vary: Accept-Encoding", so shared caches keep
 * the compressed and plain variants apart. Responses that already have a
 * Content-Encoding, ask for Cache-Control: no-transform, are small, or are
 * already-compressed media are left alone. A strong ETag on a compressed
 * body is made weak, since the bytes differ from the uncompressed variant.
 *
 * @return routing::Middleware The compression middleware handler.
 */
routing::Middleware compress();

/**
 * @brief Whether an Accept-Encoding header value allows gzip.
 */
bool accepts_gzip(std::string_view accept_encoding);

} // namespace middleware
