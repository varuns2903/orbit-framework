#include <gtest/gtest.h>
#include <orbit/config/Config.hpp>

using namespace config;

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
