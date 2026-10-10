#include <orbit/openapi/OpenApi.hpp>
#include <orbit/utils/Logger.hpp>
#include <sstream>
#include <regex>

namespace orbit::openapi {

void OpenApiRegistry::register_route(http::HttpMethod method, const std::string& path, const RouteMetadata& meta) {
    // Convert /api/users/:id to /api/users/{id}, and a trailing wildcard
    // (/files/*rest, /files/*) to /files/{rest} or /files/{path}.
    std::string swagger_path = std::regex_replace(path, std::regex("/:([^/]+)"), "/{$1}");
    swagger_path = std::regex_replace(swagger_path, std::regex("/\\*([^/]+)$"), "/{$1}");
    swagger_path = std::regex_replace(swagger_path, std::regex("/\\*$"), "/{path}");
    // A typed segment (/tasks/{id:int}) is OpenAPI's {id}; the type belongs
    // to Orbit's route matching, not the published path template. Two
    // routes that differ only in their type (/tasks/{id:int} and
    // /tasks/{id:uuid}) are dispatched separately by the router but
    // normalize to the same OpenAPI path here; OpenAPI has no way to
    // express "this path, but only for one of two shapes of {id}", so one
    // operation silently replaces the other below. Rare (same literal
    // param name, same method, differing only by type), but worth a
    // pointer to the log when it happens.
    swagger_path = std::regex_replace(swagger_path, std::regex("\\{([^:}]+):[^}]+\\}"), "{$1}");
    std::lock_guard<std::mutex> lock(mutex_);
    auto& methods = paths_[swagger_path].methods;
    if (methods.find(method) != methods.end()) {
        LOG_WARN("OpenAPI: " << path << " normalizes to the already-registered " << swagger_path
                 << " for this method; the published spec keeps only the most recently registered operation "
                 << "(the router still dispatches both routes separately)");
    }
    methods[method] = meta;
}

void OpenApiRegistry::register_schema(const std::string& name, const std::string& json_schema_body) {
    std::lock_guard<std::mutex> lock(mutex_);
    schemas_[name] = json_schema_body;
}

std::string OpenApiRegistry::method_to_string(http::HttpMethod method) const {
    switch (method) {
        case http::HttpMethod::GET: return "get";
        case http::HttpMethod::POST: return "post";
        case http::HttpMethod::PUT: return "put";
        case http::HttpMethod::DELETE: return "delete";
        case http::HttpMethod::PATCH: return "patch";
        case http::HttpMethod::OPTIONS: return "options";
        default: return "get";
    }
}

std::string OpenApiRegistry::escape_json(const std::string& s) const {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) {
            // Any other control character is invalid raw in a JSON string.
            static const char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
        else out += c;
    }
    return out;
}

std::string OpenApiRegistry::generate_swagger_json(const std::string& title, const std::string& version) const {
    // Work on copies so registration can continue while a spec is generated.
    // Schemas from the process-wide registry are included; this registry's
    // own schemas take precedence.
    std::unordered_map<std::string, std::string> schemas;
    if (this != &instance()) {
        const OpenApiRegistry& global = instance();
        std::lock_guard<std::mutex> lock(global.mutex_);
        schemas = global.schemas_;
    }
    std::map<std::string, EndpointMap> paths;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paths = paths_;
        for (const auto& [name, body] : schemas_) schemas[name] = body;
    }

    std::stringstream ss;
    ss << "{\n";
    ss << "  \"openapi\": \"3.0.0\",\n";
    ss << "  \"info\": {\n";
    ss << "    \"title\": \"" << escape_json(title) << "\",\n";
    ss << "    \"version\": \"" << escape_json(version) << "\"\n";
    ss << "  },\n";
    ss << "  \"paths\": {\n";
    
    bool first_path = true;
    for (const auto& [path, endpoint_map] : paths) {
        if (!first_path) ss << ",\n";
        first_path = false;
        
        ss << "    \"" << escape_json(path) << "\": {\n";
        
        bool first_method = true;
        for (const auto& [method, meta] : endpoint_map.methods) {
            if (!first_method) ss << ",\n";
            first_method = false;
            
            ss << "      \"" << method_to_string(method) << "\": {\n";
            ss << "        \"summary\": \"" << escape_json(meta.summary) << "\",\n";
            ss << "        \"description\": \"" << escape_json(meta.description) << "\"";
            
            if (!meta.tags.empty()) {
                ss << ",\n        \"tags\": [\n";
                for (size_t i = 0; i < meta.tags.size(); ++i) {
                    ss << "          \"" << escape_json(meta.tags[i]) << "\"";
                    if (i < meta.tags.size() - 1) ss << ",";
                    ss << "\n";
                }
                ss << "        ]";
            }
            
            if (!meta.request_body_schema.empty()) {
                ss << ",\n        \"requestBody\": {\n";
                ss << "          \"content\": {\n";
                ss << "            \"application/json\": {\n";
                ss << "              \"schema\": {\n";
                ss << "                \"$ref\": \"#/components/schemas/" << escape_json(meta.request_body_schema) << "\"\n";
                ss << "              }\n";
                ss << "            }\n";
                ss << "          }\n";
                ss << "        }";
            }
            
            ss << ",\n        \"responses\": {\n";
            bool first_response = true;
            for (const auto& [status, schema] : meta.response_schemas) {
                if (!first_response) ss << ",\n";
                first_response = false;
                
                ss << "          \"" << status << "\": {\n";
                if (schema.empty()) {
                    // A status with no body schema: description only.
                    ss << "            \"description\": \"Response for status " << status << "\"\n";
                    ss << "          }\n";
                    continue;
                }
                ss << "            \"description\": \"Response for status " << status << "\",\n";
                ss << "            \"content\": {\n";
                ss << "              \"application/json\": {\n";
                if (schema.front() == '{') {
                    // An inline schema is the value itself; wrapping it in
                    // another object made the whole spec invalid JSON.
                    ss << "                \"schema\": " << schema << "\n";
                } else {
                    ss << "                \"schema\": {\n";
                    ss << "                  \"$ref\": \"#/components/schemas/" << escape_json(schema) << "\"\n";
                    ss << "                }\n";
                }
                ss << "              }\n";
                ss << "            }\n";
                ss << "          }\n";
            }
            
            if (meta.response_schemas.empty()) {
                ss << "          \"200\": {\n";
                ss << "            \"description\": \"Success\"\n";
                ss << "          }\n";
            }
            
            ss << "        }\n";
            
            // Extract path parameters from route
            std::regex path_param_regex("\\{([^\\}]+)\\}");
            std::sregex_iterator words_begin(path.begin(), path.end(), path_param_regex);
            std::sregex_iterator words_end;
            
            if (words_begin != words_end) {
                ss << ",\n        \"parameters\": [\n";
                bool first_param = true;
                for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
                    if (!first_param) ss << ",\n";
                    first_param = false;
                    ss << "          {\n";
                    ss << "            \"name\": \"" << escape_json((*i)[1].str()) << "\",\n";
                    ss << "            \"in\": \"path\",\n";
                    ss << "            \"required\": true,\n";
                    ss << "            \"schema\": { \"type\": \"string\" }\n";
                    ss << "          }";
                }
                ss << "\n        ]\n";
            } else {
                ss << "\n";
            }
            
            ss << "      }";
        }
        
        ss << "\n    }";
    }
    
    ss << "\n  }";
    
    if (!schemas.empty()) {
        ss << ",\n  \"components\": {\n";
        ss << "    \"schemas\": {\n";
        bool first_schema = true;
        for (const auto& [name, schema_json] : schemas) {
            if (!first_schema) ss << ",\n";
            first_schema = false;
            ss << "      \"" << escape_json(name) << "\": " << schema_json;
        }
        ss << "\n    }\n";
        ss << "  }\n";
    }
    
    ss << "\n}\n";
    return ss.str();
}

} // namespace openapi
