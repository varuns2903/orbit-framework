#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/routing/Router.hpp>
#include <chrono>
#include <string>
#include <string_view>

namespace orbit::middleware {

/**
 * @brief Options for static_files().
 */
struct StaticFilesOptions {
    /// File served for a request that names a directory; empty disables it.
    std::string index = "index.html";
    /// Serve files and directories whose name starts with '.' (.env, .git/).
    bool serve_dotfiles = false;
    /// Cache-Control max-age. Zero sends "no-cache": browsers revalidate
    /// with the ETag / Last-Modified on every use.
    std::chrono::seconds max_age{0};
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that serves static files from a directory.
 *
 * Handles GET and HEAD. If a file is found, it is served and the pipeline is
 * stopped; otherwise the pipeline continues. Paths that resolve outside the
 * directory (including through symlinks) get 403, and dotfiles are hidden
 * unless StaticFilesOptions::serve_dotfiles is set.
 *
 * Responses carry ETag, Last-Modified, Cache-Control and Accept-Ranges.
 * If-None-Match / If-Modified-Since produce 304, and a single byte Range
 * produces 206 (or 416 when it lies outside the file).
 *
 * @param directory The directory containing static files.
 * @param options Index file, dotfile and caching behaviour.
 * @return routing::Middleware The static files middleware handler.
 */
routing::Middleware static_files(const std::string& directory, StaticFilesOptions options = {});

/**
 * @brief Returns the Content-Type for a file extension such as ".svg"
 *        (case-insensitive), or "application/octet-stream" if unknown.
 */
std::string_view mime_type_for_extension(std::string_view extension);

} // namespace middleware
