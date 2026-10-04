#include <orbit/config/Config.hpp>
#include <iostream>
#include <cstdlib>
#include <cstdlib>
#include <string>

namespace config {

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
                      << "  -t, --threads <num>           Number of worker threads (default: hardware concurrency)\n"
                      << "  -l, --log-level <level>       Log level (DEBUG, INFO, WARN, ERROR) (default: INFO)\n"
                      << "      --log-format <format>     text or json (default: text)\n"
                      << "  -s, --static-dir <dir>        Directory for static files\n"
                      << "  -m, --max-body-size <bytes>   Max request body size\n"
                      << "  -c, --ssl-cert <file>         SSL certificate file (enables HTTPS)\n"
                      << "  -k, --ssl-key <file>          SSL private key file\n"
                      << "  -e, --engine <engine>         Event loop engine (epoll, iouring) (default: epoll)\n"
                      << "  -v, --http-version <version>  HTTP version to enable (1.1, 2, 3) (default: 1.1)\n"
                      << "  -h, --help                    Show this help message\n";
            std::exit(0);
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-b" || arg == "--bind") && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "--backlog" && i + 1 < argc) {
            cfg.backlog = std::stoi(argv[++i]);
        } else if (arg == "--max-connections" && i + 1 < argc) {
            cfg.max_connections = static_cast<size_t>(std::stoull(argv[++i]));
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            cfg.worker_threads = static_cast<size_t>(std::stoull(argv[++i]));
        } else if (arg == "--log-format" && i + 1 < argc) {
            cfg.log_format = argv[++i];
        } else if ((arg == "-l" || arg == "--log-level") && i + 1 < argc) {
            cfg.log_level = argv[++i];
        } else if ((arg == "-s" || arg == "--static-dir") && i + 1 < argc) {
            cfg.static_dir = argv[++i];
        } else if ((arg == "-m" || arg == "--max-body-size") && i + 1 < argc) {
            cfg.max_body_size = static_cast<size_t>(std::stoull(argv[++i]));
        } else if ((arg == "-c" || arg == "--ssl-cert") && i + 1 < argc) {
            cfg.ssl_cert = argv[++i];
        } else if ((arg == "-k" || arg == "--ssl-key") && i + 1 < argc) {
            cfg.ssl_key = argv[++i];
        } else if ((arg == "-e" || arg == "--engine") && i + 1 < argc) {
            std::string engine_str = argv[++i];
            if (engine_str == "epoll") {
                cfg.engine = EventEngine::Epoll;
            } else if (engine_str == "iouring") {
                cfg.engine = EventEngine::IoUring;
            } else {
                std::cerr << "Invalid engine: " << engine_str << ". Must be 'epoll' or 'iouring'\n";
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
