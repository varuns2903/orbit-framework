#include <orbit/config/ConfigFile.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>
#else
extern char** environ;
#endif

namespace orbit::config {

namespace {

[[noreturn]] void fail(const std::string& path, const std::string& message) {
    throw ConfigError(path + ": " + message);
}

uint64_t as_count(const std::string& path, const nlohmann::json& v, uint64_t min, uint64_t max) {
    const bool whole = v.is_number_unsigned() || (v.is_number_integer() && v.get<int64_t>() >= 0);
    const uint64_t n = whole ? v.get<uint64_t>() : 0;
    if (!whole || n < min || n > max) {
        fail(path, "expected a whole number from " + std::to_string(min) + " to " + std::to_string(max) + ", not " +
                       v.dump());
    }
    return n;
}

// A count where "auto" means 0 (one per CPU).
uint64_t count_or_auto(const std::string& path, const nlohmann::json& v, uint64_t min, uint64_t max) {
    if (v.is_string() && v.get<std::string>() == "auto") return 0;
    if (!v.is_number_integer()) fail(path, "expected a whole number or \"auto\", not " + v.dump());
    return as_count(path, v, min, max);
}

std::string as_string(const std::string& path, const nlohmann::json& v) {
    if (!v.is_string()) fail(path, "expected a string, not " + v.dump());
    return v.get<std::string>();
}

bool as_bool(const std::string& path, const nlohmann::json& v) {
    if (!v.is_boolean()) fail(path, "expected true or false, not " + v.dump());
    return v.get<bool>();
}

std::chrono::seconds as_seconds(const std::string& path, const nlohmann::json& v) {
    return std::chrono::seconds(as_count(path, v, 0, std::numeric_limits<int32_t>::max()));
}

std::string one_of(const std::string& path, const std::string& value, std::initializer_list<const char*> allowed) {
    std::string list;
    for (const char* a : allowed) {
        if (value == a) return value;
        if (!list.empty()) list += ", ";
        list += std::string("\"") + a + "\"";
    }
    fail(path, "expected one of " + list + ", not \"" + value + "\"");
}

using Setter = std::function<void(ServerConfig&, const nlohmann::json&, const std::string&)>;

const std::unordered_map<std::string, Setter>& server_keys() {
    constexpr uint64_t kMax = std::numeric_limits<int64_t>::max();
    static const std::unordered_map<std::string, Setter> keys = {
        {"port", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.port = static_cast<uint16_t>(as_count(p, v, 0, 65535));
         }},
        {"host", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.host = as_string(p, v); }},
        {"backlog", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.backlog = static_cast<int>(as_count(p, v, 0, std::numeric_limits<int>::max()));
         }},
        {"max_connections", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.max_connections = static_cast<size_t>(as_count(p, v, 0, kMax));
         }},
        {"worker_threads", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.worker_threads = static_cast<size_t>(count_or_auto(p, v, 1, 4096));
         }},
        {"event_loops", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.event_loops = static_cast<size_t>(count_or_auto(p, v, 1, 1024));
         }},
        {"cpu_affinity", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.cpu_affinity = as_bool(p, v); }},
        {"log_level", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             std::string level = as_string(p, v);
             std::transform(level.begin(), level.end(), level.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
             c.log_level = one_of(p, level, {"DEBUG", "INFO", "WARN", "ERROR"});
         }},
        {"log_format", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.log_format = one_of(p, as_string(p, v), {"text", "json"});
         }},
        {"static_dir", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.static_dir = as_string(p, v); }},
        {"max_body_size", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.max_body_size = static_cast<size_t>(as_count(p, v, 0, kMax));
         }},
        {"ssl_cert", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.ssl_cert = as_string(p, v); }},
        {"ssl_key", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.ssl_key = as_string(p, v); }},
        {"sni_certificates", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             if (!v.is_array()) fail(p, "expected an array of {\"cert\": ..., \"key\": ...}");
             c.sni_certificates.clear();
             for (size_t i = 0; i < v.size(); ++i) {
                 const std::string item = p + "[" + std::to_string(i) + "]";
                 const auto& entry = v[i];
                 if (!entry.is_object()) fail(item, "expected {\"cert\": ..., \"key\": ...}");
                 for (const auto& [k, _] : entry.items()) {
                     if (k != "cert" && k != "key") fail(item + "." + k, "unknown key (expected \"cert\" and \"key\")");
                 }
                 if (!entry.contains("cert") || !entry.contains("key")) fail(item, "needs both \"cert\" and \"key\"");
                 c.sni_certificates.push_back({as_string(item + ".cert", entry["cert"]), as_string(item + ".key", entry["key"])});
             }
         }},
        {"tls_reload_interval", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.tls_reload_interval = as_seconds(p, v); }},
        {"engine", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             const std::string e = one_of(p, as_string(p, v), {"epoll", "iouring", "auto"});
             c.engine = e == "epoll" ? EventEngine::Epoll : e == "iouring" ? EventEngine::IoUring : EventEngine::Auto;
         }},
        {"http_version", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             // "1.1", "2", "3"; numbers too, as an environment value parses to one.
             std::string s = v.is_string() ? v.get<std::string>()
                           : v.is_number() ? (v.dump() == "1.1" ? "1.1" : v.dump())
                                           : as_string(p, v);
             s = one_of(p, s, {"1.1", "2", "3"});
             c.http_version = s == "1.1" ? HttpVersion::Http1_1 : s == "2" ? HttpVersion::Http2 : HttpVersion::Http3;
         }},
        {"header_timeout", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.header_timeout = as_seconds(p, v); }},
        {"keep_alive_timeout", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.keep_alive_timeout = as_seconds(p, v); }},
        {"idle_timeout", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.idle_timeout = as_seconds(p, v); }},
        {"websocket_idle_timeout", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.websocket_idle_timeout = as_seconds(p, v); }},
        {"websocket_ping_interval", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.websocket_ping_interval = as_seconds(p, v); }},
        {"shutdown_timeout", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) { c.shutdown_timeout = as_seconds(p, v); }},
        {"max_header_bytes", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.max_header_bytes = static_cast<size_t>(as_count(p, v, 1, kMax));
         }},
        {"max_request_line", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.max_request_line = static_cast<size_t>(as_count(p, v, 1, kMax));
         }},
        {"max_headers", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.max_headers = static_cast<size_t>(as_count(p, v, 1, kMax));
         }},
        {"websocket_max_message_size", [](ServerConfig& c, const nlohmann::json& v, const std::string& p) {
             c.websocket_max_message_size = static_cast<size_t>(as_count(p, v, 1, kMax));
         }},
    };
    return keys;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return s;
}

nlohmann::json env_value(const std::string& raw) {
    nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    // Numbers, booleans, arrays and objects as such; strings stay strings
    // (a quoted JSON string too, unquoted).
    if (parsed.is_discarded() || parsed.is_null()) return raw;
    return parsed;
}

// The process environment as NAME=value entries.
std::vector<std::string> environment_entries() {
    std::vector<std::string> out;
#ifdef _WIN32
    // Not _environ, which MSVC flags as deprecated (C4996, fatal with /WX).
    LPCH block = GetEnvironmentStringsA();
    if (block == nullptr) return out;
    for (const char* p = block; *p != '\0'; p += std::strlen(p) + 1) out.emplace_back(p);
    FreeEnvironmentStringsA(block);
#else
    if (environ == nullptr) return out;
    for (char** e = environ; *e != nullptr; ++e) out.emplace_back(*e);
#endif
    return out;
}

// Applies ORBIT_* variables to @p doc (see Config).
void apply_environment(nlohmann::json& doc) {
    const auto& keys = server_keys();
    for (const std::string& entry : environment_entries()) {
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string name = entry.substr(0, eq);
        if (name.size() <= 6 || lower(name.substr(0, 6)) != "orbit_") continue;
        const std::string value = entry.substr(eq + 1);
        const std::string rest = lower(name.substr(6));

        std::vector<std::string> path;
        if (rest.find("__") == std::string::npos) {
            // ORBIT_PORT: a server key, or not ours (ORBIT_TEST_ENGINE, ...).
            if (keys.find(rest) == keys.end()) continue;
            path = {"server", rest};
        } else {
            size_t start = 0;
            while (true) {
                size_t sep = rest.find("__", start);
                path.push_back(rest.substr(start, sep == std::string::npos ? sep : sep - start));
                if (sep == std::string::npos) break;
                start = sep + 2;
            }
            if (std::any_of(path.begin(), path.end(), [](const std::string& p) { return p.empty(); })) continue;
        }
        nlohmann::json* node = &doc;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            nlohmann::json& next = (*node)[path[i]];
            if (!next.is_object()) next = nlohmann::json::object();
            node = &next;
        }
        (*node)[path.back()] = env_value(value);
    }
}

} // namespace

ServerConfig Config::server_from_json(const nlohmann::json& section) {
    ServerConfig cfg;
    if (section.is_null()) return cfg;
    if (!section.is_object()) fail("server", "expected an object");
    const auto& keys = server_keys();
    for (const auto& [key, value] : section.items()) {
        auto it = keys.find(key);
        if (it == keys.end()) fail("server." + key, "unknown key");
        it->second(cfg, value, "server." + key);
    }
    return cfg;
}

Config Config::from_json(nlohmann::json document, int argc, char* argv[], ServerConfig::ParseMode mode) {
    if (document.is_null()) document = nlohmann::json::object();
    if (!document.is_object()) throw ConfigError("configuration: expected a JSON object at the top level");
    apply_environment(document);

    Config cfg;
    cfg.server = server_from_json(document.contains("server") ? document["server"] : nlohmann::json());
    if (argc > 0 && argv != nullptr) cfg.server = ServerConfig::parse(argc, argv, cfg.server, mode);
    cfg.document_ = std::move(document);
    return cfg;
}

Config Config::load(const std::string& path, int argc, char* argv[], ServerConfig::ParseMode mode) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError(path + ": cannot open the configuration file");
    std::stringstream text;
    text << in.rdbuf();
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text.str(), nullptr, true, /*ignore_comments=*/true);
    } catch (const nlohmann::json::parse_error& e) {
        throw ConfigError(path + ": not valid JSON (" + std::string(e.what()) + ")");
    }
    try {
        return from_json(std::move(document), argc, argv, mode);
    } catch (const ConfigError& e) {
        throw ConfigError(path + ": " + e.what());
    }
}

const nlohmann::json& Config::section(const std::string& name) const {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    auto it = document_.find(name);
    return it == document_.end() ? kEmpty : *it;
}

} // namespace orbit::config
