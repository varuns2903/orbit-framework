#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <orbit/http/HttpRequest.hpp>

namespace openapi {

struct RouteMetadata {
    std::string summary;
    std::string description;
    std::vector<std::string> tags;
    std::string request_body_schema;
    std::unordered_map<int, std::string> response_schemas;
};

class OpenApiRegistry {
public:
    /// Each App (root Router) has its own registry; see App::openapi().
    OpenApiRegistry() = default;

    /// Process-wide registry. Schemas registered here are included in every
    /// App's spec, so code written before per-App registries keeps working.
    static OpenApiRegistry& instance() {
        static OpenApiRegistry registry;
        return registry;
    }

    void register_route(http::HttpMethod method, const std::string& path, const RouteMetadata& meta);
    
    // Register a JSON schema (e.g. for a User object)
    void register_schema(const std::string& name, const std::string& json_schema_body);

    std::string generate_swagger_json(const std::string& title, const std::string& version) const;

private:
    struct EndpointMap {
        std::map<http::HttpMethod, RouteMetadata> methods;
    };

    // path -> endpoints
    std::map<std::string, EndpointMap> paths_;
    
    // schema_name -> schema_json_body
    std::unordered_map<std::string, std::string> schemas_;
    mutable std::mutex mutex_;
    
    std::string method_to_string(http::HttpMethod method) const;
    std::string escape_json(const std::string& s) const;
};

} // namespace openapi
