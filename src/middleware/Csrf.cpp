#include <orbit/middleware/Csrf.hpp>
#include <orbit/utils/Random.hpp>
#include <algorithm>

namespace middleware {

Csrf::Csrf(const std::string& cookie_name, const std::string& header_name)
    : cookie_name_(cookie_name), header_name_(header_name) {}

std::string Csrf::generate_random_token() {
    return utils::secure_random_hex(32);
}

bool Csrf::operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    std::string cookie_token;
    auto cookie_it = req.cookies.find(cookie_name_);
    if (cookie_it != req.cookies.end()) {
        cookie_token = cookie_it->second;
    }

    if (req.method == http::HttpMethod::GET || req.method == http::HttpMethod::HEAD || 
        req.method == http::HttpMethod::OPTIONS) {
        
        if (cookie_token.empty()) {
            std::string new_token = generate_random_token();
            std::string c_name = cookie_name_;
            writer->add_interceptor([c_name, new_token](http::HttpResponse& res) {
                http::Cookie c;
                c.name = c_name;
                c.value = new_token;
                c.path = "/";
                c.same_site = "Lax";
                res.set_cookie(c);
            });
            // Inject into request headers so route handlers/templates can read it
            req.set_header(header_name_, new_token);
        } else {
            req.set_header(header_name_, cookie_token);
        }
        return true;
    }

    // Mutating method: verify token
    std::string provided_token;
    
    // Check header
    auto header_it = req.headers.find(header_name_);
    if (header_it != req.headers.end()) {
        provided_token = std::string(header_it->second);
    } else {
        // Fallback: Check for URL-encoded body or multipart form data
        auto ct_it = req.headers.find("Content-Type");
        if (ct_it != req.headers.end()) {
            if (ct_it->second.find("application/x-www-form-urlencoded") != std::string_view::npos) {
                // A real form parse: searching the body for "_csrf=" also
                // matched inside other names, such as "x_csrf=".
                auto fields = req.form_fields();
                auto field = fields.find("_csrf");
                if (field != fields.end()) provided_token = field->second;
            } else if (ct_it->second.find("multipart/form-data") != std::string_view::npos) {
                auto form = req.form();
                if (form.fields.find("_csrf") != form.fields.end()) {
                    provided_token = form.fields["_csrf"];
                }
            }
        }
    }

    if (cookie_token.empty() || provided_token.empty() || !utils::constant_time_equals(cookie_token, provided_token)) {
        http::HttpResponse res;
        res.status(http::HttpStatus::Forbidden).send("CSRF Token Verification Failed");
        writer->send(std::move(res));
        return false;
    }

    return true;
}

} // namespace middleware
