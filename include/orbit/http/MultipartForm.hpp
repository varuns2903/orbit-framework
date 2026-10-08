#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::http {

/**
 * @brief Represents a file uploaded via multipart/form-data.
 */
struct MultipartFile {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string_view content;
};

/**
 * @brief Represents parsed multipart/form-data content.
 */
class MultipartForm {
public:
    std::unordered_map<std::string, std::string> fields;
    std::vector<MultipartFile> files;

    /**
     * @brief Parses a multipart/form-data request body.
     * @param content_type_header The Content-Type header containing the boundary.
     * @param body The raw request body.
     * @return A parsed MultipartForm object.
     */
    static MultipartForm parse(std::string_view content_type_header, std::string_view body);
};

} // namespace http
