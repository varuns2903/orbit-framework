#include <orbit/config/Config.hpp>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace orbit::config {

namespace {

// A whole number in [min, max] for a numeric flag. Anything else (not a
// number, trailing junk, a sign on a count, out of range) is a usage error,
// as an unknown engine name is, rather than an exception or a silent wrap.
long long parse_integer(const std::string& flag, const std::string& value, long long min, long long max) {
    long long n = 0;
    const char* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(value.data(), end, n);
    if (value.empty() || ec != std::errc() || ptr != end || n < min || n > max) {
        std::cerr << "Invalid value for " << flag << ": '" << value << "'. Must be a whole number from "
                  << min << " to " << max << "\n";
        std::exit(1);
    }
    return n;
}

constexpr long long kNoLimit = std::numeric_limits<long long>::max();

} // namespace

// Flags that take no value.
static const char* const kSwitches[] = {"--cpu-affinity", "-h", "--help"};

// Flags that are followed by a value.
static const char* const kValueFlags[] = {
        "-p", "--port", "-b", "--bind", "--host", "--backlog", "--max-connections", "-t", "--threads",
        "--event-loops", "--log-format", "-l", "--log-level", "-s", "--static-dir", "-m", "--max-body-size",
        "-c", "--ssl-cert", "-k", "--ssl-key", "--sni-cert", "--tls-reload-interval", "-e", "--engine",
        "-v", "--http-version"};

static bool takes_value(const std::string& arg) {
    for (const char* flag : kValueFlags) {
        if (arg == flag) return true;
    }
    return false;
}

// The known long flag closest to @p arg, if it is near enough to be a typo
// (an edit distance of at most 2, and at most half the flag's name).
static std::string closest_flag(const std::string& arg) {
    if (arg.rfind("--", 0) != 0) return "";
    auto distance = [](const std::string& a, const std::string& b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    };
    std::string best;
    size_t best_distance = 3;
    auto consider = [&](const char* flag) {
        const std::string candidate = flag;
        if (candidate.rfind("--", 0) != 0) return;
        size_t d = distance(arg, candidate);
        if (d < best_distance && d * 2 <= candidate.size() - 2) {
            best = candidate;
            best_distance = d;
        }
    };
    for (const char* flag : kValueFlags) consider(flag);
    for (const char* flag : kSwitches) consider(flag);
    return best;
}

ServerConfig ServerConfig::parse(int argc, char* argv[], ParseMode mode) {
    return parse(argc, argv, ServerConfig{}, mode);
}

ServerConfig ServerConfig::parse(int argc, char* argv[], ServerConfig base, ParseMode mode) {
    ServerConfig cfg = std::move(base);
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -p, --port <port>             Port to listen on (default: 8080)\n"
                      << "  -b, --bind, --host <host>     Address to listen on, e.g. 127.0.0.1 or :: (default: 0.0.0.0)\n"
                      << "      --backlog <num>           Listen backlog (default: SOMAXCONN)\n"
                      << "      --max-connections <num>   Connections served at once, 0 = unlimited (default: 0)\n"
                      << "  -t, --threads <num|auto>      Number of worker threads; auto = one per CPU (default: 4)\n"
                      << "      --event-loops <num|auto>  Event loops accepting and serving connections, Linux only;\n"
                      << "                                auto = one per CPU (default: auto)\n"
                      << "      --cpu-affinity            Pin each event loop to its own CPU (Linux)\n"
                      << "  -l, --log-level <level>       Log level (DEBUG, INFO, WARN, ERROR) (default: INFO)\n"
                      << "      --log-format <format>     text or json (default: text)\n"
                      << "  -s, --static-dir <dir>        Directory for static files\n"
                      << "  -m, --max-body-size <bytes>   Max request body size\n"
                      << "  -c, --ssl-cert <file>         SSL certificate file (enables HTTPS)\n"
                      << "  -k, --ssl-key <file>          SSL private key file\n"
                      << "      --sni-cert <cert> <key>   Another certificate, chosen by SNI (repeatable)\n"
                      << "      --tls-reload-interval <s> Check certificate files for changes every s seconds (default: 0, off)\n"
                      << "  -e, --engine <engine>         Event loop engine (epoll, iouring, auto) (default: epoll)\n"
                      << "  -v, --http-version <version>  HTTP version to enable (1.1, 2, 3) (default: 1.1)\n"
                      << "  -h, --help                    Show this help message\n";
            std::exit(0);
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(parse_integer(arg, argv[++i], 0, 65535));
        } else if ((arg == "-b" || arg == "--bind" || arg == "--host") && i + 1 < argc) {
            // --host matches the ServerConfig field; it used to be reported
            // as unknown and ignored, so the server bound 0.0.0.0 (#194).
            cfg.host = argv[++i];
        } else if (arg == "--backlog" && i + 1 < argc) {
            cfg.backlog = static_cast<int>(parse_integer(arg, argv[++i], 0, std::numeric_limits<int>::max()));
        } else if (arg == "--max-connections" && i + 1 < argc) {
            cfg.max_connections = static_cast<size_t>(parse_integer(arg, argv[++i], 0, kNoLimit));
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            const std::string value = argv[++i];
            cfg.worker_threads = value == "auto" ? 0 : static_cast<size_t>(parse_integer(arg, value, 1, kNoLimit));
        } else if (arg == "--event-loops" && i + 1 < argc) {
            const std::string value = argv[++i];
            cfg.event_loops = value == "auto" ? 0 : static_cast<size_t>(parse_integer(arg, value, 1, 1024));
        } else if (arg == "--cpu-affinity") {
            cfg.cpu_affinity = true;
        } else if (arg == "--log-format" && i + 1 < argc) {
            cfg.log_format = argv[++i];
        } else if ((arg == "-l" || arg == "--log-level") && i + 1 < argc) {
            cfg.log_level = argv[++i];
        } else if ((arg == "-s" || arg == "--static-dir") && i + 1 < argc) {
            cfg.static_dir = argv[++i];
        } else if ((arg == "-m" || arg == "--max-body-size") && i + 1 < argc) {
            cfg.max_body_size = static_cast<size_t>(parse_integer(arg, argv[++i], 0, kNoLimit));
        } else if ((arg == "-c" || arg == "--ssl-cert") && i + 1 < argc) {
            cfg.ssl_cert = argv[++i];
        } else if ((arg == "-k" || arg == "--ssl-key") && i + 1 < argc) {
            cfg.ssl_key = argv[++i];
        } else if (arg == "--sni-cert" && i + 2 < argc) {
            std::string cert = argv[++i];
            std::string key = argv[++i];
            cfg.sni_certificates.push_back({cert, key});
        } else if (arg == "--tls-reload-interval" && i + 1 < argc) {
            cfg.tls_reload_interval = std::chrono::seconds(parse_integer(arg, argv[++i], 0, kNoLimit));
        } else if ((arg == "-e" || arg == "--engine") && i + 1 < argc) {
            std::string engine_str = argv[++i];
            if (engine_str == "epoll") {
                cfg.engine = EventEngine::Epoll;
            } else if (engine_str == "iouring") {
                cfg.engine = EventEngine::IoUring;
            } else if (engine_str == "auto") {
                cfg.engine = EventEngine::Auto;
            } else {
                std::cerr << "Invalid engine: " << engine_str << ". Must be 'epoll', 'iouring' or 'auto'\n";
                std::exit(1);
            }
        } else if ((arg == "-v" || arg == "--http-version") && i + 1 < argc) {
            std::string version_str = argv[++i];
            if (version_str == "1.1") {
                cfg.http_version = HttpVersion::Http1_1;
            } else if (version_str == "2") {
                cfg.http_version = HttpVersion::Http2;
            } else if (version_str == "3") {
                cfg.http_version = HttpVersion::Http3;
            } else {
                std::cerr << "Invalid HTTP version: " << version_str << ". Must be '1.1', '2', or '3'\n";
                std::exit(1);
            }
        } else if (takes_value(arg)) {
            // A known flag at the end of the command line, without its value:
            // a mistake, and silently keeping the default (say, binding every
            // interface instead of the one asked for) is worse than stopping.
            std::cerr << "Missing value for " << arg << " (see --help)\n";
            std::exit(1);
        } else {
            // Fatal only when the application says the whole command line is
            // Orbit's: it may pass its own flags through to parse() as well.
            std::cerr << "Unknown argument: " << arg;
            const std::string suggestion = closest_flag(arg);
            if (!suggestion.empty()) std::cerr << " (did you mean " << suggestion << "?)";
            std::cerr << "\n";
            if (mode == ParseMode::Strict) {
                std::cerr << "See --help for the flags this server accepts.\n";
                std::exit(1);
            }
        }
    }
    return cfg;
}

} // namespace config
