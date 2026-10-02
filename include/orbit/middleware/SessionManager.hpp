#pragma once
#include <string>
#include <memory>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/database/RedisClient.hpp>

namespace middleware {

/**
 * @brief Cookie and lifetime settings for SessionManager.
 */
struct SessionOptions {
    std::string cookie_name = "session_id";
    int ttl_seconds = 86400;        ///< Idle lifetime; refreshed on every request
    bool secure = false;            ///< Set the Secure attribute (enable behind HTTPS)
    std::string same_site = "Lax";  ///< SameSite attribute: "Strict", "Lax" or "None"
};

/**
 * @brief Middleware that assigns each client a session identifier stored in Redis.
 *
 * Identifiers are 256-bit values from the system CSPRNG. An identifier sent by
 * the client is accepted only if Redis holds it, so a client (or an attacker
 * planting a cookie) cannot choose its own session identifier.
 */
class SessionManager {
public:
    SessionManager(const std::string& redis_host, int redis_port, SessionOptions options = {});
    
    /**
     * @brief Sets req.session_id to a valid session, creating one if needed.
     */
    bool operator()(http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer);

    /**
     * @brief Generates a new random session identifier (64 hex characters).
     */
    static std::string generate_session_id();

private:
    bool is_known_session(const std::string& session_id);

    std::shared_ptr<database::RedisClient> redis_;
    SessionOptions options_;
};

/**
 * @brief Creates a session middleware backed by Redis.
 */
inline auto session(const std::string& redis_host, int redis_port, SessionOptions options = {}) {
    return [manager = std::make_shared<SessionManager>(redis_host, redis_port, options)]
           (http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        return (*manager)(req, writer);
    };
}

} // namespace middleware
