#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>
#ifdef ORBIT_ENABLE_REDIS
#include <orbit/database/RedisClient.hpp>
#endif

namespace orbit::middleware {

/// Key/value data kept for one session.
using SessionData = std::unordered_map<std::string, std::string>;

/**
 * @brief Where sessions live. Implementations must be thread-safe.
 */
class SessionStore {
public:
    virtual ~SessionStore() = default;
    /// The session's data, or std::nullopt if it does not exist or expired.
    virtual std::optional<SessionData> load(const std::string& id) = 0;
    /// Creates or replaces the session; it expires after `ttl` without use.
    virtual void save(const std::string& id, const SessionData& data, std::chrono::seconds ttl) = 0;
    /// Extends the lifetime of an existing session.
    virtual void touch(const std::string& id, std::chrono::seconds ttl) = 0;
    virtual void destroy(const std::string& id) = 0;
};

/**
 * @brief In-process store. Fine for a single server; sessions are lost on
 *        restart and not shared between instances (use RedisSessionStore).
 */
class MemorySessionStore : public SessionStore {
public:
    /// @param max_sessions Upper bound on stored sessions; the ones closest to
    ///        expiry are dropped first when it is reached.
    explicit MemorySessionStore(size_t max_sessions = 100000) : max_sessions_(max_sessions) {}

    std::optional<SessionData> load(const std::string& id) override;
    void save(const std::string& id, const SessionData& data, std::chrono::seconds ttl) override;
    void touch(const std::string& id, std::chrono::seconds ttl) override;
    void destroy(const std::string& id) override;
    size_t size();

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        SessionData data;
        Clock::time_point expires;
    };
    void evict_locked(Clock::time_point now);

    size_t max_sessions_;
    std::unordered_map<std::string, Entry> sessions_;
    std::mutex mutex_;
};

#ifdef ORBIT_ENABLE_REDIS
/**
 * @brief Redis-backed store, shared by every instance pointing at the same
 *        Redis. Keys are "orbit:session:<id>" with a TTL.
 */
class RedisSessionStore : public SessionStore {
public:
    RedisSessionStore(const std::string& host, int port);
    std::optional<SessionData> load(const std::string& id) override;
    void save(const std::string& id, const SessionData& data, std::chrono::seconds ttl) override;
    void touch(const std::string& id, std::chrono::seconds ttl) override;
    void destroy(const std::string& id) override;

private:
    std::shared_ptr<database::RedisClient> redis_;
};
#endif

/**
 * @brief The current request's session (req.session).
 *
 * Changes are saved when the response is sent. Call regenerate() after a
 * login or privilege change, so an ID known before login (session fixation)
 * becomes useless, and destroy() on logout.
 */
class Session {
public:
    const std::string& id() const { return id_; }
    /// True if this request created the session.
    bool is_new() const { return is_new_; }

    std::optional<std::string> get(const std::string& key) const;
    void set(const std::string& key, std::string value);
    void erase(const std::string& key);
    const SessionData& data() const { return data_; }

    /// Issues a new ID and keeps the data; the old ID stops working.
    void regenerate();
    /// Deletes the session and expires the cookie.
    void destroy();

private:
    friend class SessionManager;
    std::string id_;
    std::string previous_id_; // set by regenerate()
    SessionData data_;
    bool is_new_ = false;
    bool dirty_ = false;
    bool destroyed_ = false;
};

struct SessionOptions {
    std::string cookie_name = "session_id";
    int ttl_seconds = 86400;        ///< Idle lifetime; refreshed on every request
    bool secure = false;            ///< Set the Secure attribute (enable behind HTTPS)
    std::string same_site = "Lax";  ///< SameSite attribute: "Strict", "Lax" or "None"
    /// Store (and send a cookie for) sessions nobody wrote to. On by default,
    /// as before; turn off so anonymous visitors and bots create no sessions.
    bool save_uninitialized = true;
};

/**
 * @ingroup middlewares
 * @brief Loads or creates the session for each request (req.session,
 *        req.session_id) and saves it when the response is sent.
 */
class SessionManager {
public:
    SessionManager(std::shared_ptr<SessionStore> store, SessionOptions options = {});
#ifdef ORBIT_ENABLE_REDIS
    /// Redis-backed; kept for compatibility with earlier releases.
    SessionManager(const std::string& redis_host, int redis_port, SessionOptions options = {});
#endif

    bool operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer);

    static std::string generate_session_id();

private:
    std::shared_ptr<SessionStore> store_;
    SessionOptions options_;
};

/// Session middleware over any store, e.g. std::make_shared<MemorySessionStore>().
inline auto session(std::shared_ptr<SessionStore> store, SessionOptions options = {}) {
    return [manager = std::make_shared<SessionManager>(std::move(store), std::move(options))]
           (http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*manager)(req, writer);
    };
}

#ifdef ORBIT_ENABLE_REDIS
/// Redis-backed session middleware (as in earlier releases).
inline auto session(const std::string& redis_host, int redis_port, SessionOptions options = {}) {
    return [manager = std::make_shared<SessionManager>(redis_host, redis_port, std::move(options))]
           (http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*manager)(req, writer);
    };
}
#endif

} // namespace middleware
