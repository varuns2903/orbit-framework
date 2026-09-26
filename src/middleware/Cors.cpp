#include <orbit/middleware/Cors.hpp>
#include <orbit/utils/Logger.hpp>
#include <algorithm>
#include <sstream>

namespace middleware {

namespace {
    std::string join(const std::vector<std::string>& vec, const std::string& delimiter) {
        if (vec.empty()) return "";
        std::ostringstream os;
        for (size_t i = 0; i < vec.size(); ++i) {
            os << vec[i];
            if (i != vec.size() - 1) os << delimiter;
        }
        return os.str();
    }
}

routing::Middleware cors(CorsOptions options) {
    std::string methods_str = join(options.allowed_methods, ", ");
    std::string headers_str = join(options.allowed_headers, ", ");
    const bool any_origin = std::find(options.allowed_origins.begin(), options.allowed_origins.end(), "*") != options.allowed_origins.end();

    // Browsers reject "Access-Control-Allow-Origin: *" together with
    // credentials, and reflecting every origin instead would let any site make
    // credentialed requests. So a wildcard never grants credentials.
    if (any_origin && options.allow_credentials) {
        LOG_WARN("CORS: allow_credentials is ignored when allowed_origins contains \"*\"; list the trusted origins explicitly");
        options.allow_credentials = false;
    }

    return [options, methods_str, headers_str, any_origin](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        auto origin_it = request.headers.find("Origin");
        const bool has_origin = origin_it != request.headers.end();
        std::string origin = has_origin ? std::string(origin_it->second) : std::string();

        bool allowed = false;
        if (has_origin) {
            allowed = any_origin || std::find(options.allowed_origins.begin(), options.allowed_origins.end(), origin) != options.allowed_origins.end();
        }

        // The response depends on Origin unless every origin gets "*".
        if (!any_origin) {
            writer->set_header("Vary", "Origin");
        }

        if (any_origin) {
            // Public resource: the same answer for everyone, Origin or not.
            writer->set_header("Access-Control-Allow-Origin", "*");
        } else if (allowed) {
            writer->set_header("Access-Control-Allow-Origin", origin);
            if (options.allow_credentials) {
                writer->set_header("Access-Control-Allow-Credentials", "true");
            }
        }

        // A preflight is an OPTIONS request from a browser announcing the
        // method it intends to use; other OPTIONS requests reach the routes.
        const bool is_preflight = request.method == http::HttpMethod::OPTIONS && has_origin &&
                                  request.headers.find("Access-Control-Request-Method") != request.headers.end();
        if (is_preflight) {
            http::HttpResponse res;
            if (allowed) {
                res.headers["Access-Control-Allow-Methods"] = methods_str;
                std::string allow_headers = headers_str;
                if (allow_headers == "*" && options.allow_credentials) {
                    // "*" is a literal header name when credentials are involved.
                    auto req_headers = request.headers.find("Access-Control-Request-Headers");
                    allow_headers = req_headers != request.headers.end() ? std::string(req_headers->second) : "";
                }
                if (!allow_headers.empty()) {
                    res.headers["Access-Control-Allow-Headers"] = allow_headers;
                }
                res.headers["Access-Control-Max-Age"] = std::to_string(options.max_age);
            }
            // A disallowed origin gets no CORS headers, so the browser blocks it.
            res.status(http::HttpStatus::NoContent).send("");
            writer->send(std::move(res));
            return false;
        }

        return true;
    };
}

} // namespace middleware
