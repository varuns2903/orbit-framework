#pragma once
#ifdef _WIN32
#undef ERROR
#endif
#include <string>
#include <iostream>
#include <sstream>
#include <mutex>
#include <functional>
#include <utility>
#include <vector>

namespace utils {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

/// Text: human-readable lines (colored on a terminal). Json: one JSON object
/// per line, for log collectors (Loki, ELK, CloudWatch, ...).
enum class LogFormat { Text, Json };

using LogFields = std::vector<std::pair<std::string, std::string>>;

class Logger {
public:
    static void init(const std::string& level_str);
    static void log(LogLevel level, const char* file, int line, const std::string& msg);
    /// A message with structured fields: JSON keys in Json format,
    /// key=value pairs in Text format.
    static void log_fields(LogLevel level, const char* file, int line, const std::string& msg,
                           const LogFields& fields);

    static void set_format(LogFormat format);
    /// Parses "text" or "json" (case-insensitive); anything else keeps the current format.
    static void set_format(const std::string& format);
    static LogFormat format();

    /// Where finished lines go (without the trailing newline). nullptr
    /// (the default) writes to stdout.
    using Sink = std::function<void(const std::string& line)>;
    static void set_sink(Sink sink);

    static LogLevel current_level;
private:
    static std::mutex log_mutex;
    static std::string level_to_string(LogLevel level, bool color);
};

} // namespace utils

// The macros expand where they are used, and <windows.h> (pulled in by curl,
// winsock, ...) defines ERROR as a macro. They therefore refer to these
// constants, which are declared here where ERROR has been #undef'd. The
// stream variable has an unusual name so it cannot shadow a local variable
// (MSVC treats that warning as an error).
namespace utils::log_levels {
inline constexpr LogLevel kDebug = LogLevel::DEBUG;
inline constexpr LogLevel kInfo = LogLevel::INFO;
inline constexpr LogLevel kWarn = LogLevel::WARN;
inline constexpr LogLevel kError = LogLevel::ERROR;
} // namespace utils::log_levels

#define ORBIT_LOG_AT(level, msg) do { if (utils::Logger::current_level <= (level)) { std::ostringstream orbit_log_stream_; orbit_log_stream_ << msg; utils::Logger::log((level), __FILE__, __LINE__, orbit_log_stream_.str()); } } while(0)
#define LOG_DEBUG(msg) ORBIT_LOG_AT(utils::log_levels::kDebug, msg)
#define LOG_INFO(msg)  ORBIT_LOG_AT(utils::log_levels::kInfo, msg)
#define LOG_WARN(msg)  ORBIT_LOG_AT(utils::log_levels::kWarn, msg)
#define LOG_ERROR(msg) ORBIT_LOG_AT(utils::log_levels::kError, msg)
