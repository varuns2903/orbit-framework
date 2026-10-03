#include <orbit/server/App.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <orbit/openapi/OpenApi.hpp>
#include <orbit/network/ConnectionPool.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <csignal>
#include <atomic>
#include <string>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <fcntl.h>
#include <vector>

namespace server {

static App* g_app = nullptr;

// The only thing the signal handler does is store the signal number. A
// lock-free atomic store is async-signal-safe; logging, locking, allocating
// or forking from a handler can deadlock (e.g. if the signal interrupts a
// thread that holds the logger's mutex).
static std::atomic<int> g_pending_signal{0};
static_assert(std::atomic<int>::is_always_lock_free, "signal flag must be lock-free");

void signal_handler(int signum) {
    g_pending_signal.store(signum, std::memory_order_relaxed);
}

App::App(const config::ServerConfig& config) : config_(config) {
    network::initialize_platform_networking();
    utils::Logger::init(config.log_level);
    if (!config_.ssl_cert.empty() && !config_.ssl_key.empty()) {
        tls_context_ = std::make_unique<network::TlsContext>(config_.ssl_cert, config_.ssl_key, config_.http_version);
    }
}

App::~App() {
    stop();
    network::cleanup_platform_networking();
}

void App::on_error(routing::ErrorHandler handler) {
    router_.on_error(std::move(handler));
}

App& App::use(routing::Middleware m) {
    router_.use(std::move(m));
    return *this;
}

App& App::get(const std::string& path, routing::RouteHandler handler) {
    router_.get(path, std::move(handler));
    return *this;
}

App& App::get(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.get(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::post(const std::string& path, routing::RouteHandler handler) {
    router_.post(path, std::move(handler));
    return *this;
}

App& App::post(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.post(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::put(const std::string& path, routing::RouteHandler handler) {
    router_.put(path, std::move(handler));
    return *this;
}

App& App::put(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.put(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::patch(const std::string& path, routing::RouteHandler handler) {
    router_.patch(path, std::move(handler));
    return *this;
}

App& App::patch(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.patch(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::del(const std::string& path, routing::RouteHandler handler) {
    router_.del(path, std::move(handler));
    return *this;
}

App& App::del(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.del(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::options(const std::string& path, routing::RouteHandler handler) {
    router_.options(path, std::move(handler));
    return *this;
}

App& App::options(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler) {
    router_.options(path, std::move(mws), std::move(handler));
    return *this;
}

App& App::enable_metrics(const std::string& path) {
    this->get(path, [](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(utils::PrometheusRegistry::get_instance().expose(), "text/plain; version=0.0.4");
        res->send(std::move(response));
    });
    return *this;
}

App& App::enable_openapi(const std::string& title, const std::string& version, const std::string& docs_path, const std::string& json_path) {
    this->get(json_path, [title, version](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        std::string json = openapi::OpenApiRegistry::instance().generate_swagger_json(title, version);
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(json, "application/json");
        res->send(std::move(response));
    });

    this->get(docs_path, [json_path](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        std::string html = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Swagger UI</title>
    <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5.11.0/swagger-ui.css" />
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://unpkg.com/swagger-ui-dist@5.11.0/swagger-ui-bundle.js"></script>
    <script>
    window.onload = () => {
        window.ui = SwaggerUIBundle({
            url: ')" + json_path + R"(',
            dom_id: '#swagger-ui',
        });
    };
    </script>
</body>
</html>)";
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(html, "text/html");
        res->send(std::move(response));
    });
    
    return *this;
}

void App::listen() {
    g_app = this;
    
#ifndef _WIN32
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGUSR2, &action, nullptr);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, nullptr);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    listener_ = std::make_unique<Listener>(config_.port);
    listener_->start();
    
    network::UdpSocket* pass_quic_socket = nullptr;
    QuicConnectionManager* pass_quic_manager = nullptr;

#ifdef ORBIT_ENABLE_HTTP3
    if (config_.http_version == config::HttpVersion::Http3) {
        if (!tls_context_) {
            LOG_WARN("HTTP/3 QUIC is enabled but no SSL certificates were provided. QUIC requires TLS. Disabling QUIC.");
        } else {
            quic_socket_ = std::make_unique<network::UdpSocket>();
            quic_socket_->set_non_blocking();
            quic_socket_->bind(config_.port);
            quic_manager_ = std::make_unique<QuicConnectionManager>(*quic_socket_, tls_context_->get());
            pass_quic_socket = quic_socket_.get();
            pass_quic_manager = quic_manager_.get();
            LOG_INFO("HTTP/3 QUIC enabled on UDP port " << config_.port);
        }
    }
#else
    if (config_.http_version == config::HttpVersion::Http3) {
        LOG_WARN("HTTP/3 QUIC was requested but the framework was compiled with ORBIT_ENABLE_HTTP3=OFF. Disabling QUIC.");
    }
#endif
    
    event_loop_ = std::make_unique<EventLoop>(*listener_, router_, config_, tls_context_.get(), pass_quic_socket, pass_quic_manager);

    // Act on signals from the loop thread, where logging and forking are safe.
    event_loop_->set_tick_hook([this]() {
        int signum = g_pending_signal.exchange(0, std::memory_order_relaxed);
        if (signum == 0) return;
#ifndef _WIN32
        if (signum == SIGUSR2) {
            LOG_INFO("SIGUSR2 received. Initiating zero-downtime hot reload...");
            hot_reload();
            return;
        }
#endif
        LOG_INFO("Interrupt signal (" << signum << ") received. Stopping server gracefully...");
        stop();
    });
    
    LOG_INFO("App started listening on port " << config_.port);
    event_loop_->run();
}

void App::stop() {
    if (event_loop_) {
        event_loop_->stop();
    }
}

void App::hot_reload() {
#ifndef _WIN32
    // Build argv before fork(): after fork() in a multi-threaded process the
    // child may only call async-signal-safe functions, so no allocation or
    // logging there.
    std::vector<std::string> args_str;
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::string cmdline;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            cmdline.append(buf, static_cast<size_t>(n));
        }
        close(fd);
        size_t pos = 0;
        while (pos < cmdline.size()) {
            size_t end = cmdline.find('\0', pos);
            if (end == std::string::npos) end = cmdline.size();
            args_str.push_back(cmdline.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    if (args_str.empty()) {
        LOG_ERROR("Hot reload needs /proc/self/cmdline, which is not available on this platform");
        return;
    }
    std::vector<char*> args;
    for (auto& s : args_str) args.push_back(s.data());
    args.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: close inherited descriptors and replace the process image.
        for (int i = 3; i < 1024; ++i) {
            close(i);
        }
        execv("/proc/self/exe", args.data());
        _exit(127);
    } else if (pid > 0) {
        // Parent: Stop accepting new connections and drain
        if (event_loop_) {
            event_loop_->stop_accepting();
        }
    } else {
        LOG_ERROR("Failed to fork for hot reload: " << strerror(errno));
    }
#else
    LOG_ERROR("Hot reload not supported on Windows");
#endif
}

} // namespace server
