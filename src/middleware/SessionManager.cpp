#include <orbit/middleware/SessionManager.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/utils/Logger.hpp>
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

// --- MemorySessionStore ---

std::optional<SessionData> MemorySessionStore::load(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return std::nullopt;
    if (it->second.expires <= Clock::now()) {
        sessions_.erase(it);
        return std::nullopt;
    }
    return it->second.data;
}

void MemorySessionStore::save(const std::string& id, const SessionData& data, std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    if (sessions_.find(id) == sessions_.end() && sessions_.size() >= max_sessions_) {
        evict_locked(now);
    }
    sessions_[id] = Entry{data, now + ttl};
}

void MemorySessionStore::touch(const std::string& id, std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) it->second.expires = Clock::now() + ttl;
}

void MemorySessionStore::destroy(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
}

size_t MemorySessionStore::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

// Makes room for one more: drops expired sessions, then (if still full) the
// one closest to expiry.
void MemorySessionStore::evict_locked(Clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expires <= now) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    while (!sessions_.empty() && sessions_.size() >= max_sessions_) {
        auto oldest = sessions_.begin();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->second.expires < oldest->second.expires) oldest = it;
        }
        sessions_.erase(oldest);
    }
}

// --- RedisSessionStore ---

#ifdef ORBIT_ENABLE_REDIS
RedisSessionStore::RedisSessionStore(const std::string& host, int port)
    : redis_(std::make_shared<database::RedisClient>(host, port)) {}

std::optional<SessionData> RedisSessionStore::load(const std::string& id) {
    auto value = redis_->get(kKeyPrefix + id);
    if (!value) return std::nullopt;
    SessionData data;
    // Sessions written before data was stored hold the placeholder "1".
    auto json = nlohmann::json::parse(*value, nullptr, false);
    if (json.is_object()) {
        for (auto& [key, val] : json.items()) {
            if (val.is_string()) data[key] = val.get<std::string>();
        }
    }
    return data;
}

void RedisSessionStore::save(const std::string& id, const SessionData& data, std::chrono::seconds ttl) {
    nlohmann::json json = nlohmann::json::object();
    for (const auto& [key, val] : data) json[key] = val;
    redis_->set(kKeyPrefix + id, json.dump(), static_cast<int>(ttl.count()));
}

void RedisSessionStore::touch(const std::string& id, std::chrono::seconds ttl) {
    redis_->expire(kKeyPrefix + id, static_cast<int>(ttl.count()));
}

void RedisSessionStore::destroy(const std::string& id) {
    redis_->del(kKeyPrefix + id);
}
#endif

// --- Session ---

std::optional<std::string> Session::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

void Session::set(const std::string& key, std::string value) {
    data_[key] = std::move(value);
    dirty_ = true;
}

void Session::erase(const std::string& key) {
    if (data_.erase(key)) dirty_ = true;
}

void Session::regenerate() {
    if (previous_id_.empty() && !is_new_) previous_id_ = id_;
    id_ = SessionManager::generate_session_id();
    dirty_ = true;
}

void Session::destroy() {
    destroyed_ = true;
    data_.clear();
}

// --- SessionManager ---

SessionManager::SessionManager(std::shared_ptr<SessionStore> store, SessionOptions options)
    : store_(std::move(store)), options_(std::move(options)) {}

#ifdef ORBIT_ENABLE_REDIS
SessionManager::SessionManager(const std::string& redis_host, int redis_port, SessionOptions options)
    : SessionManager(std::make_shared<RedisSessionStore>(redis_host, redis_port), std::move(options)) {}
#endif

std::string SessionManager::generate_session_id() {
    return utils::secure_random_hex(32);
}

bool SessionManager::operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
    const std::chrono::seconds ttl(options_.ttl_seconds);
    auto session = std::make_shared<Session>();

    auto it = req.cookies.find(options_.cookie_name);
    std::optional<SessionData> existing;
    if (it != req.cookies.end() && is_well_formed(it->second)) {
        existing = store_->load(it->second);
    }
    if (existing) {
        session->id_ = it->second;
        session->data_ = std::move(*existing);
        store_->touch(session->id_, ttl); // idle lifetime restarts with each request
    } else {
        // Unknown, expired or malformed ids are never adopted: the server
        // always chooses the id (no session fixation via a planted cookie).
        session->id_ = generate_session_id();
        session->is_new_ = true;
        if (options_.save_uninitialized) {
            store_->save(session->id_, {}, ttl);
        }
    }

    req.session = session;
    req.session_id = session->id_;

    // Persist changes and update the cookie when the response goes out.
    std::shared_ptr<SessionStore> store = store_;
    SessionOptions opts = options_;
    writer->add_interceptor([session, store, opts, ttl](http::HttpResponse& res) {
        auto cookie = [&opts](const std::string& value, int max_age) {
            http::Cookie c;
            c.name = opts.cookie_name;
            c.value = value;
            c.path = "/";
            c.http_only = true;
            c.secure = opts.secure;
            c.same_site = opts.same_site;
            c.max_age = max_age;
            return c;
        };

        if (session->destroyed_) {
            store->destroy(session->id_);
            if (!session->previous_id_.empty()) store->destroy(session->previous_id_);
            if (!session->is_new_ || !session->previous_id_.empty()) res.set_cookie(cookie("", 0));
            return;
        }
        if (!session->previous_id_.empty()) store->destroy(session->previous_id_);

        const bool id_changed = session->is_new_ || !session->previous_id_.empty();
        if (session->dirty_ || (session->is_new_ && opts.save_uninitialized)) {
            store->save(session->id_, session->data_, ttl);
        }
        // A new or regenerated id is only worth a cookie if it was stored.
        if (id_changed && (session->dirty_ || opts.save_uninitialized)) {
            res.set_cookie(cookie(session->id_, opts.ttl_seconds));
        }
    });
    return true; // Continue pipeline
}

} // namespace middleware
