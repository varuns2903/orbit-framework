#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>

namespace http {

MultipartForm MultipartForm::parse(std::string_view content_type_header, std::string_view body) {
    MultipartForm form;
    
    std::string boundary = multipart_boundary(content_type_header);
    if (boundary.empty()) return form;

    std::string delimiter = "--" + boundary;

    size_t pos = body.find(delimiter);
    if (pos == std::string_view::npos) return form;
    pos += delimiter.length();

    while (pos < body.length()) {
        size_t next_pos = body.find(delimiter, pos);
        if (next_pos == std::string_view::npos) break;

        if (pos + 2 <= body.length() && body[pos] == '\r' && body[pos+1] == '\n') {
            pos += 2;
        }
        
        size_t part_end = next_pos;
        if (part_end >= 2 && body[part_end - 2] == '\r' && body[part_end - 1] == '\n') {
            part_end -= 2;
        }

        std::string_view part = body.substr(pos, part_end - pos);
        
        size_t header_end = part.find("\r\n\r\n");
        if (header_end != std::string_view::npos) {
            std::string_view headers = part.substr(0, header_end);
            std::string_view part_body = part.substr(header_end + 4);
            
            std::string name;
            std::string filename;
            std::string content_type;

            size_t h_start = 0;
            while (h_start < headers.length()) {
                size_t h_end = headers.find("\r\n", h_start);
                if (h_end == std::string_view::npos) h_end = headers.length();
                
                std::string_view header_line = headers.substr(h_start, h_end - h_start);
                
                if (header_line.starts_with("Content-Disposition:")) {
                    size_t name_pos = header_line.find("name=\"");
                    if (name_pos != std::string_view::npos) {
                        size_t name_end = header_line.find("\"", name_pos + 6);
                        if (name_end != std::string_view::npos) {
                            name = header_line.substr(name_pos + 6, name_end - (name_pos + 6));
                        }
                    }
                    size_t fn_pos = header_line.find("filename=\"");
                    if (fn_pos != std::string_view::npos) {
                        size_t fn_end = header_line.find("\"", fn_pos + 10);
                        if (fn_end != std::string_view::npos) {
                            filename = header_line.substr(fn_pos + 10, fn_end - (fn_pos + 10));
                        }
                    }
                } else if (header_line.starts_with("Content-Type:")) {
                    size_t ct_pos = header_line.find(":");
                    if (ct_pos != std::string_view::npos) {
                        content_type = header_line.substr(ct_pos + 1);
                        while (!content_type.empty() && content_type.front() == ' ') content_type.erase(0, 1);
                    }
                }
                h_start = h_end + 2;
            }
            
            if (!filename.empty()) {
                form.files.push_back({name, filename, content_type, part_body});
            } else if (!name.empty()) {
                form.fields[name] = std::string(part_body);
            }
        }
        
        pos = next_pos + delimiter.length();
        if (pos + 2 <= body.length() && body.substr(pos, 2) == "--") {
            break;
        }
    }
    
    return form;
}
} // namespace http
