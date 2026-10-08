#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <string_view>
#include <memory>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <functional>

namespace orbit::middleware {

/**
 * @ingroup middlewares
 * @brief Cross-Site Request Forgery (CSRF) protection middleware.
 */
class Csrf {
public:
    /**
     * @brief Constructs a new Csrf middleware instance.
     * 
     * @param cookie_name The name of the cookie to store the CSRF token.
     * @param header_name The name of the header expected in requests.
     */
    Csrf(const std::string& cookie_name = "csrf_token", const std::string& header_name = "X-CSRF-Token");
    
    /**
     * @brief Middleware execution operator.
     * 
     * @param req The HTTP request.
     * @param writer The HTTP response writer.
     * @return true If the request should continue to the next handler.
     * @return false If the request should be blocked.
     */
    bool operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer);

    /**
     * @brief Generates a random CSRF token.
     * 
     * @return std::string The generated token.
     */
    static std::string generate_random_token();

private:
    std::string cookie_name_;
    std::string header_name_;
};

/**
 * @ingroup middlewares
 * @brief Helper to create a CSRF protection middleware handler.
 * 
 * @param cookie_name The name of the cookie to store the CSRF token.
 * @param header_name The name of the header expected in requests.
 * @return A middleware handler function.
 */
inline auto csrf_protection(const std::string& cookie_name = "csrf_token", const std::string& header_name = "X-CSRF-Token") {
    return [c = std::make_shared<Csrf>(cookie_name, header_name)](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*c)(req, writer);
    };
}

} // namespace middleware
