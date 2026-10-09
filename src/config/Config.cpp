#include <orbit/config/Config.hpp>
#include <charconv>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <string>
#include <system_error>

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

ServerConfig ServerConfig::parse(int argc, char* argv[]) {
    ServerConfig cfg;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -p, --port <port>             Port to listen on (default: 8080)\n"
                      << "  -b, --bind <host>             Address to listen on, e.g. 127.0.0.1 or :: (default: 0.0.0.0)\n"
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
        } else if ((arg == "-b" || arg == "--bind") && i + 1 < argc) {
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
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
        }
    }
    return cfg;
}

} // namespace config
