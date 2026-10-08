#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/utils/Logger.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

// The logger is process-wide (#190): creating an App must not reset a level
// or format that was set before it, and lines must reach stdout as they are
// logged, even when stdout is a file or a pipe.

using orbit::utils::LogFormat;
using orbit::utils::Logger;
using orbit::utils::LogLevel;

namespace {

struct RestoreLogger {
    ~RestoreLogger() {
        Logger::init("INFO");
        Logger::set_format(LogFormat::Text);
    }
};

} // namespace

TEST(LoggerProcessTest, AppWithDefaultConfigKeepsAnEarlierLevelAndFormat) {
    RestoreLogger restore;
    Logger::init("DEBUG");
    Logger::set_format(LogFormat::Json);

    orbit::config::ServerConfig cfg; // log_level "INFO", log_format "text" by default
    orbit::server::App app(cfg);

    EXPECT_EQ(Logger::current_level, LogLevel::DEBUG);
    EXPECT_EQ(Logger::format(), LogFormat::Json);
}

TEST(LoggerProcessTest, AppAppliesExplicitSettings) {
    RestoreLogger restore;
    orbit::config::ServerConfig cfg;
    cfg.log_level = "WARN";
    cfg.log_format = "json";
    orbit::server::App app(cfg);

    EXPECT_EQ(Logger::current_level, LogLevel::WARN);
    EXPECT_EQ(Logger::format(), LogFormat::Json);

    // A later App with the defaults leaves them alone.
    orbit::server::App second{orbit::config::ServerConfig{}};
    EXPECT_EQ(Logger::current_level, LogLevel::WARN);
    EXPECT_EQ(Logger::format(), LogFormat::Json);
}

#ifndef _WIN32
TEST(LoggerProcessTest, LinesAreWrittenImmediatelyWhenStdoutIsAFile) {
    RestoreLogger restore;
    Logger::init("INFO");
    Logger::set_format(LogFormat::Text);

    char path[] = "/tmp/orbit_logger_flush_XXXXXX";
    int file = mkstemp(path);
    ASSERT_NE(file, -1);

    std::fflush(stdout);
    int saved = dup(fileno(stdout));
    ASSERT_NE(saved, -1);
    ASSERT_NE(dup2(file, fileno(stdout)), -1);

    LOG_INFO("line that must not wait in a buffer");

    // Read the file before restoring stdout (restoring flushes nothing of
    // the logger's, but keep the order unambiguous).
    std::ifstream in(path);
    std::stringstream content;
    content << in.rdbuf();

    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    close(file);
    unlink(path);

    EXPECT_NE(content.str().find("line that must not wait in a buffer"), std::string::npos)
        << "the log line was still buffered when it should have been written";
}
#endif
