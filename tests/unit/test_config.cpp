#include <gtest/gtest.h>
#include <orbit/config/Config.hpp>
#include <chrono>
#include <string>
#include <vector>

using namespace orbit::config;

TEST(ServerConfigTest, DefaultPort) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.port, 8080);
}

TEST(ServerConfigTest, DefaultWorkerThreads) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.worker_threads, 4u);
}

TEST(ServerConfigTest, DefaultLogLevel) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.log_level, "INFO");
}

TEST(ServerConfigTest, DefaultStaticDir) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.static_dir, "./public");
}

TEST(ServerConfigTest, DefaultMaxBodySize) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.max_body_size, 10485760u); // 10 MB
}

TEST(ServerConfigTest, DefaultSslEmpty) {
    ServerConfig cfg;
    EXPECT_TRUE(cfg.ssl_cert.empty());
    EXPECT_TRUE(cfg.ssl_key.empty());
}

TEST(ServerConfigTest, CustomPort) {
    ServerConfig cfg;
    cfg.port = 3000;
    EXPECT_EQ(cfg.port, 3000);
}

TEST(ServerConfigTest, CustomWorkerThreads) {
    ServerConfig cfg;
    cfg.worker_threads = 16;
    EXPECT_EQ(cfg.worker_threads, 16u);
}

TEST(ServerConfigTest, DefaultEngine) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.engine, EventEngine::Epoll);
}

TEST(ServerConfigTest, DefaultHttpVersion) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.http_version, HttpVersion::Http1_1);
}

TEST(ServerConfigTest, SslConfig) {
    ServerConfig cfg;
    cfg.ssl_cert = "/path/to/cert.pem";
    cfg.ssl_key = "/path/to/key.pem";
    EXPECT_EQ(cfg.ssl_cert, "/path/to/cert.pem");
    EXPECT_EQ(cfg.ssl_key, "/path/to/key.pem");
}

// --- Command-line parsing ---

namespace {

// Runs ServerConfig::parse over the given arguments, after a program name.
ServerConfig parse_args(std::vector<std::string> args) {
    args.insert(args.begin(), "orbit");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return ServerConfig::parse(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(ServerConfigParseTest, NoArgumentsGiveTheDefaults) {
    ServerConfig parsed = parse_args({});
    ServerConfig defaults;
    EXPECT_EQ(parsed.port, defaults.port);
    EXPECT_EQ(parsed.host, defaults.host);
    EXPECT_EQ(parsed.worker_threads, defaults.worker_threads);
    EXPECT_EQ(parsed.engine, defaults.engine);
    EXPECT_EQ(parsed.http_version, defaults.http_version);
}

TEST(ServerConfigParseTest, LongFlags) {
    ServerConfig cfg = parse_args({
        "--port", "9090", "--bind", "::1", "--backlog", "64", "--max-connections", "500",
        "--threads", "8", "--log-level", "DEBUG", "--log-format", "json", "--static-dir", "/srv/www",
        "--max-body-size", "1024", "--ssl-cert", "cert.pem", "--ssl-key", "key.pem",
        "--tls-reload-interval", "60", "--engine", "iouring", "--http-version", "2",
    });
    EXPECT_EQ(cfg.port, 9090);
    EXPECT_EQ(cfg.host, "::1");
    EXPECT_EQ(cfg.backlog, 64);
    EXPECT_EQ(cfg.max_connections, 500u);
    EXPECT_EQ(cfg.worker_threads, 8u);
    EXPECT_EQ(cfg.log_level, "DEBUG");
    EXPECT_EQ(cfg.log_format, "json");
    EXPECT_EQ(cfg.static_dir, "/srv/www");
    EXPECT_EQ(cfg.max_body_size, 1024u);
    EXPECT_EQ(cfg.ssl_cert, "cert.pem");
    EXPECT_EQ(cfg.ssl_key, "key.pem");
    EXPECT_EQ(cfg.tls_reload_interval, std::chrono::seconds(60));
    EXPECT_EQ(cfg.engine, EventEngine::IoUring);
    EXPECT_EQ(cfg.http_version, HttpVersion::Http2);
}

TEST(ServerConfigParseTest, ShortFlags) {
    ServerConfig cfg = parse_args({
        "-p", "3000", "-b", "127.0.0.1", "-t", "2", "-l", "WARN", "-s", "./assets",
        "-m", "2048", "-c", "c.pem", "-k", "k.pem", "-e", "epoll", "-v", "3",
    });
    EXPECT_EQ(cfg.port, 3000);
    EXPECT_EQ(cfg.host, "127.0.0.1");
    EXPECT_EQ(cfg.worker_threads, 2u);
    EXPECT_EQ(cfg.log_level, "WARN");
    EXPECT_EQ(cfg.static_dir, "./assets");
    EXPECT_EQ(cfg.max_body_size, 2048u);
    EXPECT_EQ(cfg.ssl_cert, "c.pem");
    EXPECT_EQ(cfg.ssl_key, "k.pem");
    EXPECT_EQ(cfg.engine, EventEngine::Epoll);
    EXPECT_EQ(cfg.http_version, HttpVersion::Http3);
}

TEST(ServerConfigParseTest, EventLoops) {
    EXPECT_EQ(parse_args({}).event_loops, 0u) << "auto (one per CPU) by default";
    EXPECT_EQ(parse_args({"--event-loops", "8"}).event_loops, 8u);
}

TEST(ServerConfigParseTest, HttpVersionOneOneCanBeChosen) {
    EXPECT_EQ(parse_args({"-v", "2"}).http_version, HttpVersion::Http2);
    EXPECT_EQ(parse_args({"-v", "2", "-v", "1.1"}).http_version, HttpVersion::Http1_1);
}

TEST(ServerConfigParseTest, SniCertIsRepeatable) {
    ServerConfig cfg = parse_args({"--sni-cert", "a.pem", "a.key", "--sni-cert", "b.pem", "b.key"});
    ASSERT_EQ(cfg.sni_certificates.size(), 2u);
    EXPECT_EQ(cfg.sni_certificates[0].cert_file, "a.pem");
    EXPECT_EQ(cfg.sni_certificates[0].key_file, "a.key");
    EXPECT_EQ(cfg.sni_certificates[1].cert_file, "b.pem");
    EXPECT_EQ(cfg.sni_certificates[1].key_file, "b.key");
}

TEST(ServerConfigParseTest, LaterFlagWins) {
    EXPECT_EQ(parse_args({"--port", "1000", "-p", "2000"}).port, 2000);
}

TEST(ServerConfigParseTest, PortBoundsAreAccepted) {
    EXPECT_EQ(parse_args({"--port", "0"}).port, 0);
    EXPECT_EQ(parse_args({"--port", "65535"}).port, 65535);
}

// Unknown arguments are reported and skipped; parsing carries on.
TEST(ServerConfigParseTest, UnknownArgumentIsSkipped) {
    testing::internal::CaptureStderr();
    ServerConfig cfg = parse_args({"--frobnicate", "--port", "4000"});
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(cfg.port, 4000);
    EXPECT_NE(err.find("Unknown argument: --frobnicate"), std::string::npos) << err;
}

// A flag missing its value is not applied, so the default stays.
TEST(ServerConfigParseTest, FlagWithoutValueLeavesTheDefault) {
    testing::internal::CaptureStderr();
    ServerConfig cfg = parse_args({"--port"});
    testing::internal::GetCapturedStderr();
    EXPECT_EQ(cfg.port, ServerConfig{}.port);

    testing::internal::CaptureStderr();
    cfg = parse_args({"--sni-cert", "only-a-cert.pem"});
    testing::internal::GetCapturedStderr();
    EXPECT_TRUE(cfg.sni_certificates.empty());
}

class ServerConfigParseDeathTest : public testing::Test {
protected:
    void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

TEST_F(ServerConfigParseDeathTest, HelpExitsSuccessfully) {
    EXPECT_EXIT(parse_args({"--help"}), testing::ExitedWithCode(0), "");
    EXPECT_EXIT(parse_args({"-p", "1", "-h"}), testing::ExitedWithCode(0), "");
}

TEST_F(ServerConfigParseDeathTest, InvalidEngineExits) {
    EXPECT_EXIT(parse_args({"--engine", "kqueue"}), testing::ExitedWithCode(1), "Invalid engine: kqueue");
}

TEST_F(ServerConfigParseDeathTest, InvalidHttpVersionExits) {
    EXPECT_EXIT(parse_args({"--http-version", "2.0"}), testing::ExitedWithCode(1), "Invalid HTTP version: 2.0");
}

TEST_F(ServerConfigParseDeathTest, PortOutOfRangeExitsInsteadOfWrapping) {
    EXPECT_EXIT(parse_args({"--port", "65536"}), testing::ExitedWithCode(1), "Invalid value for --port: '65536'");
    EXPECT_EXIT(parse_args({"--port", "70000"}), testing::ExitedWithCode(1), "Invalid value for --port");
    EXPECT_EXIT(parse_args({"-p", "-1"}), testing::ExitedWithCode(1), "Invalid value for -p");
}

TEST_F(ServerConfigParseDeathTest, NonNumericValueExitsInsteadOfThrowing) {
    EXPECT_EXIT(parse_args({"--port", "http"}), testing::ExitedWithCode(1), "Invalid value for --port: 'http'");
    EXPECT_EXIT(parse_args({"--port", "80abc"}), testing::ExitedWithCode(1), "Invalid value for --port");
    EXPECT_EXIT(parse_args({"--port", ""}), testing::ExitedWithCode(1), "Invalid value for --port");
    EXPECT_EXIT(parse_args({"--backlog", "1.5"}), testing::ExitedWithCode(1), "Invalid value for --backlog");
    EXPECT_EXIT(parse_args({"--tls-reload-interval", "1m"}), testing::ExitedWithCode(1), "Invalid value for --tls-reload-interval");
}

TEST_F(ServerConfigParseDeathTest, NegativeCountsAreRejected) {
    EXPECT_EXIT(parse_args({"--threads", "-1"}), testing::ExitedWithCode(1), "Invalid value for --threads");
    EXPECT_EXIT(parse_args({"--max-connections", "-5"}), testing::ExitedWithCode(1), "Invalid value for --max-connections");
    EXPECT_EXIT(parse_args({"--max-body-size", "-1"}), testing::ExitedWithCode(1), "Invalid value for --max-body-size");
    EXPECT_EXIT(parse_args({"--backlog", "-1"}), testing::ExitedWithCode(1), "Invalid value for --backlog");
    EXPECT_EXIT(parse_args({"--tls-reload-interval", "-1"}), testing::ExitedWithCode(1), "Invalid value for --tls-reload-interval");
}

TEST_F(ServerConfigParseDeathTest, ZeroEventLoopsIsRejected) {
    EXPECT_EXIT(parse_args({"--event-loops", "0"}), testing::ExitedWithCode(1), "Invalid value for --event-loops");
}

TEST_F(ServerConfigParseDeathTest, ZeroWorkerThreadsIsRejected) {
    EXPECT_EXIT(parse_args({"-t", "0"}), testing::ExitedWithCode(1), "Invalid value for -t: '0'");
}

// --- "auto" settings (#173) ---

TEST(ServerConfigParseTest, AutoValues) {
    auto cfg = parse_args({"--threads", "auto", "--event-loops", "auto", "--engine", "auto", "--cpu-affinity"});
    EXPECT_EQ(cfg.worker_threads, 0u);
    EXPECT_EQ(cfg.event_loops, 0u);
    EXPECT_EQ(cfg.engine, EventEngine::Auto);
    EXPECT_TRUE(cfg.cpu_affinity);
}

TEST(ServerConfigParseTest, AffinityIsOffByDefault) {
    auto cfg = parse_args({});
    EXPECT_FALSE(cfg.cpu_affinity);
    EXPECT_EQ(cfg.event_loops, 0u) << "event loops default to auto";
}
