#pragma once
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/MultipartStreamParser.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace http {

/// A file part saved to disk by receive_multipart().
struct UploadedFile {
    std::string field;        ///< The form field name.
    std::string filename;     ///< Chosen by the client: never use it as a path.
    std::string content_type; ///< As sent by the client.
    std::string path;         ///< Where the file was saved (owner-only permissions).
};

/// The result of receive_multipart().
struct MultipartUpload {
    std::unordered_map<std::string, std::string> fields;
    /// Saved files. They belong to the application, which should move or
    /// delete them; discard() deletes them all.
    std::vector<UploadedFile> files;
    /// Empty on success. On failure no files are left on disk.
    std::string error;

    bool ok() const { return error.empty(); }
    /// Deletes every saved file (for example after rejecting the upload).
    void discard();
};

/**
 * @brief Receives a multipart/form-data body, streaming file parts to disk.
 *
 * Use it from a stream route (Router::add_stream_route), so the body is not
 * buffered in memory first. Fields are collected and files are written
 * under `limits.upload_dir` as they arrive.
 *
 * @p on_complete is called exactly once: at once if the Content-Type is not
 * multipart/form-data with a boundary, otherwise when the body has ended.
 * The result is a failure if a limit was exceeded or the body is malformed
 * or truncated (no closing boundary); every file already saved is then
 * deleted before the call.
 *
 * @code
 * app.group("/api", [](routing::Router& api) {
 *     api.add_stream_route(http::HttpMethod::POST, "/upload",
 *         [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
 *             http::receive_multipart(req, writer, [writer](http::MultipartUpload& upload) {
 *                 http::HttpResponse res;
 *                 if (!upload.ok()) res.status(http::HttpStatus::BadRequest).send(upload.error);
 *                 else res.send("saved " + std::to_string(upload.files.size()) + " file(s)");
 *                 writer->send(std::move(res));
 *             });
 *         });
 * });
 * @endcode
 */
void receive_multipart(const HttpRequest& req, std::shared_ptr<ResponseWriter> writer,
                       std::function<void(MultipartUpload&)> on_complete, MultipartLimits limits = {});

} // namespace http
