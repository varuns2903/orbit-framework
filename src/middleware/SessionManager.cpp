#include <orbit/middleware/SessionManager.hpp>
#include <orbit/utils/Random.hpp>

namespace middleware {

namespace {

constexpr const char* kKeyPrefix = "orbit:session:";

bool is_well_formed(const std::string& id) {
    if (id.size() != 64) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

} // namespace

SessionManager::SessionManager(const std::string& redis_host, int redis_port, SessionOptions options)
    : redis_(std::make_shared<database::RedisClient>(redis_host, redis_port)), options_(std::move(options)) {}

std::string SessionManager::generate_session_id() {
    return utils::secure_random_hex(32);
}

bool SessionManager::is_known_session(const std::string& session_id) {
    // Refresh the idle lifetime of sessions that exist; unknown ids are refused.
    return redis_->expire(kKeyPrefix + session_id, options_.ttl_seconds);
}

bool SessionManager::operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    std::string session_id;
    auto it = req.cookies.find(options_.cookie_name);
    if (it != req.cookies.end() && is_well_formed(it->second) && is_known_session(it->second)) {
        session_id = it->second;
    }
    
    if (session_id.empty()) {
        session_id = generate_session_id();
        redis_->set(kKeyPrefix + session_id, "1", options_.ttl_seconds);

        SessionOptions opts = options_;
        writer->add_interceptor([session_id, opts](http::HttpResponse& res) {
            http::Cookie c;
            c.name = opts.cookie_name;
            c.value = session_id;
            c.path = "/";
            c.http_only = true;
            c.secure = opts.secure;
            c.same_site = opts.same_site;
            c.max_age = opts.ttl_seconds;
            res.set_cookie(c);
        });
    }
    
    req.session_id = session_id;
    return true; // Continue pipeline
}

} // namespace middleware
