#include <orbit/http/MultipartUpload.hpp>

#include <cctype>
#include <cstdio>

namespace http {

void MultipartUpload::discard() {
    for (const auto& file : files) std::remove(file.path.c_str());
    files.clear();
}

void receive_multipart(const HttpRequest& req, std::shared_ptr<ResponseWriter> writer,
                       std::function<void(MultipartUpload&)> on_complete, MultipartLimits limits) {
    auto upload = std::make_shared<MultipartUpload>();
    auto fail = [upload, on_complete](const std::string& reason) {
        upload->discard();
        upload->fields.clear();
        upload->error = reason;
        on_complete(*upload);
    };

    std::string boundary;
    auto ct = req.headers.find("Content-Type");
    if (ct != req.headers.end() && ct->second.size() >= 19) {
        std::string_view type = ct->second;
        bool is_form_data = true;
        constexpr std::string_view kFormData = "multipart/form-data";
        for (size_t i = 0; i < kFormData.size(); ++i) {
            is_form_data = is_form_data && std::tolower(static_cast<unsigned char>(type[i])) == kFormData[i];
        }
        if (is_form_data) boundary = multipart_boundary(type);
    }
    if (boundary.empty()) {
        // Nothing is read: the caller answers, and the connection drops the body.
        fail("expected multipart/form-data with a boundary");
        return;
    }

    auto parser = std::make_shared<MultipartStreamParser>(
        boundary,
        [upload](const std::string& name, const std::string& value) { upload->fields[name] = value; },
        [upload](const std::string& name, const std::string& filename, const std::string& content_type,
                 const std::string& path) {
            upload->files.push_back({name, filename, content_type, path});
        },
        std::move(limits));

    writer->read_body_stream(
        [parser](std::string_view chunk) { parser->feed(chunk); },
        [parser, upload, on_complete, fail]() {
            parser->end();
            if (parser->failed()) {
                fail(parser->error());
            } else if (!parser->complete()) {
                fail("multipart body ended before its closing boundary");
            } else {
                on_complete(*upload);
            }
        });
}

} // namespace http
