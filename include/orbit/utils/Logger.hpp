#pragma once
#ifdef _WIN32
#undef ERROR
#endif
#include <string>
#include <iostream>
#include <sstream>
#include <mutex>

namespace utils {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger {
public:
    static void init(const std::string& level_str);
    static void log(LogLevel level, const char* file, int line, const std::string& msg);
    
    static LogLevel current_level;
private:
    static std::mutex log_mutex;
    static std::string level_to_string(LogLevel level);
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
