#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/json.hpp>
#include <stdexcept>
#include <string>

namespace orbit::config {

/// A configuration problem, naming the key path ("server.port: ...").
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief An application's configuration: a JSON file, then `ORBIT_*`
 *        environment variables, then command-line flags (later wins).
 *
 * The "server" section becomes a ServerConfig; its keys are the
 * ServerConfig field names, with durations in seconds and the engine
 * ("epoll", "iouring", "auto") and HTTP version ("1.1", "2", "3") as
 * strings. Every other section is the application's, read with section().
 *
 * @code
 * // orbit.json
 * {
 *   "server":   { "port": 8080, "worker_threads": "auto", "header_timeout": 5 },
 *   "database": { "url": "postgres://localhost/app", "pool_size": 8 }
 * }
 *
 * auto cfg = orbit::config::Config::load("orbit.json", argc, argv);
 * orbit::server::App app(cfg.server);
 * auto db_url = cfg.section("database").value("url", "");
 * @endcode
 *
 * Environment variables:
 * - `ORBIT_<KEY>` sets server.<key> for a key the server section knows
 *   (`ORBIT_PORT=9000`, `ORBIT_LOG_LEVEL=DEBUG`); others are ignored.
 * - `ORBIT_<SECTION>__<KEY>` (double underscore between levels) sets any
 *   path (`ORBIT_DATABASE__URL=...`, `ORBIT_SERVER__PORT=9000`).
 * Names are matched case-insensitively. A value that parses as JSON (a
 * number, true/false, an array) is used as such, anything else as a string.
 *
 * Validation is strict: an unknown key in the server section, or a value of
 * the wrong type or out of range, throws ConfigError naming the key.
 */
class Config {
public:
    ServerConfig server;

    /**
     * @brief Reads @p path, applies the environment, then the command line
     *        (as ServerConfig::parse(); pass argc = 0 for none).
     * @throws ConfigError if the file cannot be read or parsed, or is invalid.
     */
    static Config load(const std::string& path, int argc = 0, char* argv[] = nullptr,
                       ServerConfig::ParseMode mode = ServerConfig::ParseMode::Lenient);

    /// As load(), from a document already in memory (tests, generated config).
    static Config from_json(nlohmann::json document, int argc = 0, char* argv[] = nullptr,
                            ServerConfig::ParseMode mode = ServerConfig::ParseMode::Lenient);

    /// An application section, with environment overrides applied; an empty
    /// object if the document has none.
    const nlohmann::json& section(const std::string& name) const;

    /// The whole document after environment overrides.
    const nlohmann::json& document() const { return document_; }

    /// The server section as a ServerConfig (strictly validated).
    static ServerConfig server_from_json(const nlohmann::json& server_section);

private:
    nlohmann::json document_ = nlohmann::json::object();
};

} // namespace orbit::config
