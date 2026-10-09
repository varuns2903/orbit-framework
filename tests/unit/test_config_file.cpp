#include <gtest/gtest.h>
#include <orbit/config/ConfigFile.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// orbit::config::Config (#204): a JSON file, ORBIT_* environment variables
// and command-line flags, in that order of precedence.

using orbit::config::Config;
using orbit::config::ConfigError;
using orbit::config::EventEngine;
using orbit::config::HttpVersion;

namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// Sets an environment variable for one test.
struct ScopedEnv {
    std::string name;
    ScopedEnv(const char* n, const char* v) : name(n) { set_env(n, v); }
    ~ScopedEnv() { unset_env(name.c_str()); }
};

std::string error_of(const nlohmann::json& doc) {
    try {
        Config::from_json(doc);
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "<no error>";
}

} // namespace

TEST(ConfigFileTest, ServerSectionSetsTheFields) {
    Config cfg = Config::from_json(nlohmann::json::parse(R"({
        "server": {
            "port": 9001, "host": "127.0.0.1", "worker_threads": "auto", "event_loops": 2,
            "cpu_affinity": true, "log_level": "debug", "log_format": "json",
            "max_body_size": 1048576, "header_timeout": 5, "shutdown_timeout": 3,
            "engine": "iouring", "http_version": "2",
            "sni_certificates": [{"cert": "a.pem", "key": "a.key"}]
        }
    })"));
    EXPECT_EQ(cfg.server.port, 9001);
    EXPECT_EQ(cfg.server.host, "127.0.0.1");
    EXPECT_EQ(cfg.server.worker_threads, 0u) << "auto";
    EXPECT_EQ(cfg.server.event_loops, 2u);
    EXPECT_TRUE(cfg.server.cpu_affinity);
    EXPECT_EQ(cfg.server.log_level, "DEBUG");
    EXPECT_EQ(cfg.server.log_format, "json");
    EXPECT_EQ(cfg.server.max_body_size, 1048576u);
    EXPECT_EQ(cfg.server.header_timeout, std::chrono::seconds(5));
    EXPECT_EQ(cfg.server.shutdown_timeout, std::chrono::seconds(3));
    EXPECT_EQ(cfg.server.engine, EventEngine::IoUring);
    EXPECT_EQ(cfg.server.http_version, HttpVersion::Http2);
    ASSERT_EQ(cfg.server.sni_certificates.size(), 1u);
    EXPECT_EQ(cfg.server.sni_certificates[0].key_file, "a.key");
    // Untouched fields keep their defaults.
    EXPECT_EQ(cfg.server.idle_timeout, orbit::config::ServerConfig{}.idle_timeout);
}

TEST(ConfigFileTest, ValidationNamesTheKey) {
    EXPECT_EQ(error_of({{"server", {{"portt", 80}}}}), "server.portt: unknown key");
    EXPECT_NE(error_of({{"server", {{"port", "80"}}}}).find("server.port: expected a whole number from 0 to 65535"),
              std::string::npos);
    EXPECT_NE(error_of({{"server", {{"port", 70000}}}}).find("server.port"), std::string::npos);
    EXPECT_NE(error_of({{"server", {{"worker_threads", -1}}}}).find("server.worker_threads"), std::string::npos);
    EXPECT_NE(error_of({{"server", {{"engine", "kqueue"}}}}).find("server.engine: expected one of"), std::string::npos);
    EXPECT_NE(error_of({{"server", {{"cpu_affinity", "yes"}}}}).find("server.cpu_affinity: expected true or false"),
              std::string::npos);
    EXPECT_NE(error_of({{"server", {{"sni_certificates", {{{"cert", "a"}}}}}}}).find("server.sni_certificates[0]"),
              std::string::npos);
    EXPECT_NE(error_of(nlohmann::json::array()).find("top level"), std::string::npos);
}

TEST(ConfigFileTest, ApplicationSectionsAreReadAsIs) {
    Config cfg = Config::from_json({{"database", {{"url", "postgres://db/app"}, {"pool_size", 8}}}});
    EXPECT_EQ(cfg.section("database").value("url", ""), "postgres://db/app");
    EXPECT_EQ(cfg.section("database").value("pool_size", 0), 8);
    EXPECT_TRUE(cfg.section("missing").is_object());
    EXPECT_TRUE(cfg.section("missing").empty());
    EXPECT_EQ(cfg.server.port, orbit::config::ServerConfig{}.port);
}

TEST(ConfigFileTest, EnvironmentOverridesTheFile) {
    ScopedEnv port("ORBIT_PORT", "9100");
    ScopedEnv level("ORBIT_LOG_LEVEL", "WARN");
    ScopedEnv affinity("ORBIT_CPU_AFFINITY", "true");
    ScopedEnv loops("ORBIT_SERVER__EVENT_LOOPS", "auto");
    ScopedEnv version("ORBIT_HTTP_VERSION", "1.1");
    ScopedEnv url("ORBIT_DATABASE__URL", "postgres://env/app");
    ScopedEnv nested("ORBIT_CACHE__REDIS__PORT", "6380");
    ScopedEnv foreign("ORBIT_SOMETHING_ELSE", "ignored"); // not a server key, no "__": not ours

    Config cfg = Config::from_json(nlohmann::json::parse(R"({
        "server": {"port": 8000, "event_loops": 4, "http_version": "2"},
        "database": {"url": "postgres://file/app", "pool_size": 4}
    })"));
    EXPECT_EQ(cfg.server.port, 9100);
    EXPECT_EQ(cfg.server.log_level, "WARN");
    EXPECT_TRUE(cfg.server.cpu_affinity);
    EXPECT_EQ(cfg.server.event_loops, 0u);
    EXPECT_EQ(cfg.server.http_version, HttpVersion::Http1_1) << "an env value that parses as a number";
    EXPECT_EQ(cfg.section("database").value("url", ""), "postgres://env/app");
    EXPECT_EQ(cfg.section("database").value("pool_size", 0), 4) << "the rest of the section stays";
    EXPECT_EQ(cfg.section("cache")["redis"].value("port", 0), 6380);
}

TEST(ConfigFileTest, BadEnvironmentValuesAreReported) {
    ScopedEnv bogus("ORBIT_SERVER__BOGUS", "1");
    EXPECT_EQ(error_of(nlohmann::json::object()), "server.bogus: unknown key");
}

TEST(ConfigFileTest, FlagsOverrideTheEnvironment) {
    ScopedEnv port("ORBIT_PORT", "9100");
    std::vector<std::string> args = {"app", "--port", "7000"};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    Config cfg = Config::from_json({{"server", {{"port", 8000}, {"host", "10.0.0.1"}}}},
                                   static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(cfg.server.port, 7000);
    EXPECT_EQ(cfg.server.host, "10.0.0.1") << "file settings the flags do not touch stay";
}

TEST(ConfigFileTest, LoadReadsAFileWithComments) {
    auto path = std::filesystem::temp_directory_path() / "orbit_config_test.json";
    std::ofstream(path) << "{\n  // comments are allowed\n  \"server\": {\"port\": 8181}\n}\n";
    Config cfg = Config::load(path.string());
    EXPECT_EQ(cfg.server.port, 8181);

    std::ofstream(path, std::ios::trunc) << "{\"server\": {\"port\": }";
    try {
        Config::load(path.string());
        ADD_FAILURE() << "invalid JSON accepted";
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find("not valid JSON"), std::string::npos) << e.what();
    }

    std::ofstream(path, std::ios::trunc) << "{\"server\": {\"port\": true}}";
    try {
        Config::load(path.string());
        ADD_FAILURE() << "bad type accepted";
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find(path.string() + ": server.port"), std::string::npos) << e.what();
    }
    std::filesystem::remove(path);

    EXPECT_THROW(Config::load("/no/such/orbit.json"), ConfigError);
}
