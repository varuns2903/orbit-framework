#include <orbit/middleware/SecurityHeaders.hpp>

#include <utility>
#include <vector>

namespace middleware {

routing::Middleware security_headers(SecurityHeadersOptions options) {
    // Built once; every request gets the same set.
    std::vector<std::pair<std::string, std::string>> headers;
    if (options.hsts) {
        std::string hsts = "max-age=" + std::to_string(options.hsts_max_age.count());
        if (options.hsts_include_subdomains) hsts += "; includeSubDomains";
        if (options.hsts_preload) hsts += "; preload";
        headers.emplace_back("Strict-Transport-Security", hsts);
    }
    auto add = [&headers](const char* name, const std::string& value) {
        if (!value.empty()) headers.emplace_back(name, value);
    };
    add("X-Content-Type-Options", options.content_type_options);
    add("X-Frame-Options", options.frame_options);
    add("Referrer-Policy", options.referrer_policy);
    add("Cross-Origin-Opener-Policy", options.cross_origin_opener_policy);
    add("Content-Security-Policy", options.content_security_policy);
    add("Permissions-Policy", options.permissions_policy);

    return [headers = std::move(headers)](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        // Default headers: applied to the response unless the handler sets its own.
        for (const auto& [name, value] : headers) writer->set_header(name, value);
        return true;
    };
}

} // namespace middleware
