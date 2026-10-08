#include <orbit/utils/Logger.hpp>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <iomanip>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace orbit::utils {

LogLevel Logger::current_level = LogLevel::INFO;
std::mutex Logger::log_mutex;

void Logger::init(const std::string& level_str) {
    if (level_str == "DEBUG") current_level = LogLevel::DEBUG;
    else if (level_str == "INFO") current_level = LogLevel::INFO;
    else if (level_str == "WARN") current_level = LogLevel::WARN;
    else if (level_str == "ERROR") current_level = LogLevel::ERROR;
}

namespace {

LogFormat g_format = LogFormat::Text;
Logger::Sink g_sink;

bool stdout_is_terminal() {
#ifdef _WIN32
    static const bool tty = _isatty(_fileno(stdout)) != 0;
#else
    static const bool tty = isatty(fileno(stdout)) != 0;
#endif
    return tty;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "debug";
        case LogLevel::INFO: return "info";
        case LogLevel::WARN: return "warn";
        case LogLevel::ERROR: return "error";
    }
    return "unknown";
}

std::string base_name(const char* file) {
    std::string f(file);
    size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? f : f.substr(slash + 1);
}

} // namespace

void Logger::set_format(LogFormat format) {
    std::lock_guard<std::mutex> lock(log_mutex);
    g_format = format;
}

void Logger::set_format(const std::string& format) {
    std::string f;
    for (char c : format) f += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (f == "json") set_format(LogFormat::Json);
    else if (f == "text") set_format(LogFormat::Text);
}

LogFormat Logger::format() {
    std::lock_guard<std::mutex> lock(log_mutex);
    return g_format;
}

void Logger::set_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(log_mutex);
    g_sink = std::move(sink);
}

std::string Logger::level_to_string(LogLevel level, bool color) {
    if (!color) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }
        return "UNKNOWN";
    }
    switch (level) {
        case LogLevel::DEBUG: return "\033[36mDEBUG\033[0m"; // Cyan
        case LogLevel::INFO:  return "\033[32mINFO \033[0m"; // Green
        case LogLevel::WARN:  return "\033[33mWARN \033[0m"; // Yellow
        case LogLevel::ERROR: return "\033[31mERROR\033[0m"; // Red
        default: return "UNKNOWN";
    }
}

void Logger::log(LogLevel level, const char* file, int line, const std::string& msg) {
    log_fields(level, file, line, msg, {});
}

void Logger::log_fields(LogLevel level, const char* file, int line, const std::string& msg, const LogFields& fields) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::string file_str = base_name(file);

    std::lock_guard<std::mutex> lock(log_mutex);
    std::ostringstream out;
    if (g_format == LogFormat::Json) {
        // UTC, ISO 8601: unambiguous for collectors.
        struct tm ti {};
#ifdef _WIN32
        gmtime_s(&ti, &time);
#else
        gmtime_r(&time, &ti);
#endif
        out << "{\"ts\":\"" << std::put_time(&ti, "%Y-%m-%dT%H:%M:%S") << "." << std::setfill('0') << std::setw(3)
            << ms.count() << "Z\",\"level\":\"" << level_name(level) << "\",\"source\":\""
            << json_escape(file_str) << ":" << line << "\",\"msg\":\"" << json_escape(msg) << "\"";
        for (const auto& [key, value] : fields) {
            out << ",\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
        }
        out << "}";
    } else {
        struct tm ti {};
#ifdef _WIN32
        localtime_s(&ti, &time);
#else
        localtime_r(&time, &ti);
#endif
        const bool color = !g_sink && stdout_is_terminal();
        out << "[" << std::put_time(&ti, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3)
            << ms.count() << "] "
            << "[" << level_to_string(level, color) << "] "
            << "[" << file_str << ":" << line << "] " << msg;
        for (const auto& [key, value] : fields) {
            bool quote = value.empty() || value.find_first_of(" \t\"=") != std::string::npos;
            out << " " << key << "=" << (quote ? "\"" + json_escape(value) + "\"" : value);
        }
    }

    if (g_sink) {
        g_sink(out.str());
    } else {
        std::cout << out.str() << "\n";
    }
}

} // namespace utils
