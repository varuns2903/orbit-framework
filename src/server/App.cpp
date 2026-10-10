#include <orbit/server/App.hpp>
#include "Scheduler.hpp"
#include <stdexcept>
#include <algorithm>
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
#include <thread>
#ifdef __linux__
#include <liburing.h>
#include <pthread.h>
#include <sched.h>
#endif

namespace orbit::server {

// The only thing the signal handler does is record the signal. Lock-free
// atomic operations are async-signal-safe; logging, locking, allocating or
// forking from a handler can deadlock (e.g. if the signal interrupts a
// thread that holds the logger's mutex).
//
// Every running App watches the sequence number, so a signal reaches all of
// them; consuming a single flag let only one App see it.
static std::atomic<int> g_last_signal{0};
static std::atomic<unsigned> g_signal_seq{0};
static_assert(std::atomic<int>::is_always_lock_free, "signal state must be lock-free");
static_assert(std::atomic<unsigned>::is_always_lock_free, "signal state must be lock-free");

void signal_handler(int signum) {
    g_last_signal.store(signum, std::memory_order_relaxed);
    g_signal_seq.fetch_add(1, std::memory_order_release);
}

namespace {

// CPUs this process may run on: its affinity mask on Linux (containers,
// taskset, cgroups), otherwise all of them.
std::vector<int> allowed_cpus() {
    std::vector<int> cpus;
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (size_t c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &set)) cpus.push_back(static_cast<int>(c));
        }
    }
#endif
    if (cpus.empty()) {
        unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned c = 0; c < n; ++c) cpus.push_back(static_cast<int>(c));
    }
    return cpus;
}

// io_uring with fast poll (Linux 5.7+): what EventEngine::Auto requires.
bool io_uring_usable() {
#ifdef __linux__
    struct io_uring ring;
    struct io_uring_params params{};
    if (io_uring_queue_init_params(4, &ring, &params) < 0) return false;
    bool ok = (params.features & IORING_FEAT_FAST_POLL) != 0;
    io_uring_queue_exit(&ring);
    return ok;
#else
    return false;
#endif
}

void pin_current_thread([[maybe_unused]] int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<size_t>(cpu), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        LOG_WARN("Could not pin an event loop to CPU " << cpu);
    }
#endif
}

const char* engine_name(config::EventEngine engine) {
    switch (engine) {
        case config::EventEngine::Epoll: return "epoll";
        case config::EventEngine::IoUring: return "io_uring";
        case config::EventEngine::Auto: return "auto";
    }
    return "?";
}

} // namespace

App::App(const config::ServerConfig& config) : config_(config), scheduler_(std::make_unique<Scheduler>()) {
    network::initialize_platform_networking();
    // The logger is process-wide. Only settings that differ from the
    // defaults are applied, so an App with a default config cannot undo an
    // earlier Logger::init() (or another App's --log-level) by being created.
    if (config.log_level != config::ServerConfig{}.log_level) utils::Logger::init(config.log_level);
    if (config.log_format != config::ServerConfig{}.log_format) utils::Logger::set_format(config.log_format);
    if (!config_.ssl_cert.empty() && !config_.ssl_key.empty()) {
        tls_context_ = std::make_unique<network::TlsContext>(
            config::TlsCertificate{config_.ssl_cert, config_.ssl_key}, config_.sni_certificates, config_.http_version);
    }
}

App::~App() {
    stop();
    scheduler_->stop();
    network::cleanup_platform_networking();
}

void App::on_error(routing::ErrorHandler handler) {
    router_.on_error(std::move(handler));
}

App& App::use(routing::Middleware m) {
    router_.use(std::move(m));
    return *this;
}

App& App::use(const std::string& prefix, routing::Middleware m) {
    router_.use(prefix, std::move(m));
    return *this;
}

App& App::not_found(routing::RouteHandler handler) {
    router_.not_found(std::move(handler));
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

namespace {

// Subresource Integrity hashes of swagger-ui-dist 5.11.0 (identical on unpkg
// and jsDelivr). Changing the Swagger UI version means recomputing them:
//   curl -fsSL <url> | openssl dgst -sha384 -binary | openssl base64 -A
constexpr const char* kSwaggerCssIntegrity =
    "sha384-+yyzNgM3K92sROwsXxYCxaiLWxWJ0G+v/9A+qIZ2rgefKgkdcmJI+L601cqPD/Ut";
constexpr const char* kSwaggerBundleIntegrity =
    "sha384-qn5tagrAjZi8cSmvZ+k3zk4+eDEEUcP9myuR2J6V+/H6rne++v6ChO7EeHAEzqxQ";

// Escapes a value for an HTML attribute.
std::string html_attribute(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

// Escapes a value for a single-quoted JavaScript string. "<" is escaped
// too, so the result is also safe inside an HTML <script> element.
std::string js_string(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '<': out += "\\x3c"; break; // no "</script>" inside the string
            default: out += c;
        }
    }
    return out;
}

} // namespace

App& App::enable_openapi(const std::string& title, const std::string& version, const std::string& docs_path,
                         const std::string& json_path, const std::string& assets_url) {
    openapi::OpenApiRegistry* registry = &openapi();
    this->get(json_path, [title, version, registry](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        std::string json = registry->generate_swagger_json(title, version);
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(json, "application/json");
        res->send(std::move(response));
    });

    std::string base = assets_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    std::string init_path = docs_path;
    while (!init_path.empty() && init_path.back() == '/') init_path.pop_back();
    init_path += "/init.js";

    // Built once: the page only depends on the arguments above. It has no
    // inline script, so it works under a Content-Security-Policy whose
    // script-src allows only 'self' and the asset origin (#192).
    const std::string html = R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Swagger UI</title>
    <link rel="stylesheet" href=")" + html_attribute(base + "/swagger-ui.css") + R"("
          integrity=")" + kSwaggerCssIntegrity + R"(" crossorigin="anonymous" referrerpolicy="no-referrer" />
</head>
<body>
    <div id="swagger-ui"></div>
    <script src=")" + html_attribute(base + "/swagger-ui-bundle.js") + R"("
            integrity=")" + kSwaggerBundleIntegrity + R"(" crossorigin="anonymous" referrerpolicy="no-referrer"></script>
    <script src=")" + html_attribute(init_path) + R"("></script>
</body>
</html>)";

    const std::string init_js = R"(window.onload = () => {
    window.ui = SwaggerUIBundle({
        url: ')" + js_string(json_path) + R"(',
        dom_id: '#swagger-ui',
    });
};
)";

    this->get(docs_path, [html](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(html, "text/html");
        res->send(std::move(response));
    });
    this->get(init_path, [init_js](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        http::HttpResponse response;
        response.status(http::HttpStatus::OK);
        response.set_body(init_js, "text/javascript");
        res->send(std::move(response));
    });
    
    return *this;
}

void App::listen() {
    // Fail fast, before any socket is touched: a route registered twice
    // (the same method and exact pattern text) silently keeps only one of
    // the two, with no other warning (#201).
    if (auto problems = router_.validate_routes(); !problems.empty()) {
        std::string message = "duplicate routes:";
        for (const auto& problem : problems) message += "\n  " + problem;
        throw std::invalid_argument(message);
    }

    // Signals raised before this App started are not for it.
    seen_signal_seq_ = g_signal_seq.load(std::memory_order_acquire);

#ifndef _WIN32
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGUSR2, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, nullptr);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif

    // "auto" settings become concrete here, once, before anything uses them.
    const std::vector<int> cpus = allowed_cpus();
    const bool auto_loops = config_.event_loops == 0;
    if (auto_loops) config_.event_loops = cpus.size();
    if (config_.worker_threads == 0) config_.worker_threads = cpus.size();
    if (config_.engine == config::EventEngine::Auto) {
        config_.engine = io_uring_usable() ? config::EventEngine::IoUring : config::EventEngine::Epoll;
    }

    size_t loop_count = std::max<size_t>(1, config_.event_loops);
#ifndef __linux__
    // Only Linux spreads connections across SO_REUSEPORT sockets; elsewhere
    // the last socket bound would take them all.
    if (loop_count > 1) {
        // "auto" (the default) quietly means one loop here.
        if (!auto_loops) LOG_WARN("event_loops = " << loop_count << " is supported on Linux only; using 1");
        loop_count = 1;
    }
#endif
    config_.event_loops = loop_count; // what effective_config() reports
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        event_loops_.clear();
        listeners_.clear();
    }
    // The first socket may be given port 0 and get a free one; the others
    // join it on the port it got.
    listeners_.push_back(std::make_unique<Listener>(config_.host, config_.port, config_.backlog));
    listeners_.front()->start();
    for (size_t i = 1; i < loop_count; ++i) {
        listeners_.push_back(std::make_unique<Listener>(config_.host, listeners_.front()->port(), config_.backlog));
        listeners_.back()->start();
    }
    
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
    
    {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        thread_pool_ = std::make_unique<concurrency::ThreadPool>(config_.worker_threads);
        for (size_t i = 0; i < loop_count; ++i) {
            // HTTP/3 has one UDP socket; it stays on the first loop.
            event_loops_.push_back(std::make_unique<EventLoop>(
                *listeners_[i], router_, config_, *thread_pool_, tls_context_.get(),
                i == 0 ? pass_quic_socket : nullptr, i == 0 ? pass_quic_manager : nullptr));
        }
        if (loop_count > 1 && config_.max_connections > 0) {
            // max_connections is one limit for the whole App.
            open_connections_ = 0;
            for (auto& loop : event_loops_) loop->share_connection_limit(&open_connections_);
        }

        // Act on signals from the loop thread, where logging and forking are safe.
        event_loops_.front()->set_tick_hook([this]() {
            // Renewed certificate files (certbot, cert-manager) are picked up
            // without a signal.
            if (tls_context_ && config_.tls_reload_interval.count() > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now >= next_tls_check_) {
                    next_tls_check_ = now + config_.tls_reload_interval;
                    if (tls_context_->files_changed()) tls_context_->reload();
                }
            }

            unsigned seq = g_signal_seq.load(std::memory_order_acquire);
            if (seq == seen_signal_seq_) return;
            seen_signal_seq_ = seq;
            int signum = g_last_signal.load(std::memory_order_relaxed);
#ifndef _WIN32
            if (signum == SIGHUP) {
                LOG_INFO("SIGHUP received. Reloading TLS certificates...");
                reload_tls();
                return;
            }
            if (signum == SIGUSR2) {
                LOG_INFO("SIGUSR2 received. Initiating zero-downtime hot reload...");
                hot_reload();
                return;
            }
#endif
            if (draining_) {
                LOG_WARN("Signal " << signum << " received again; stopping immediately.");
                stop();
                return;
            }
            LOG_INFO("Signal " << signum << " received. Shutting down gracefully (up to "
                     << config_.shutdown_timeout.count() << " s; send it again to stop at once)...");
            shutdown();
        });

        // stop() may already have been called, e.g. by a test tearing down
        // before this thread got here.
        if (stop_requested_) {
            for (auto& loop : event_loops_) loop->stop();
        }
        for (size_t i = 1; i < event_loops_.size(); ++i) {
            EventLoop* loop = event_loops_[i].get();
            const bool pin = config_.cpu_affinity;
            const int cpu = cpus[i % cpus.size()];
            loop_threads_.emplace_back([loop, pin, cpu] {
                if (pin) pin_current_thread(cpu);
                loop->run();
            });
        }
    }
    if (config_.cpu_affinity) pin_current_thread(cpus.front());

    LOG_INFO("App started listening on port " << listeners_.front()->port()
             << (loop_count > 1 ? " with " + std::to_string(loop_count) + " event loops" : std::string()));
#ifdef __linux__
    const std::string engine = engine_name(config_.engine);
#else
    // epoll/io_uring apply to Linux; elsewhere the platform's proactor runs.
    const std::string engine = std::string("platform default (configured: ") + engine_name(config_.engine) + ")";
#endif
    LOG_INFO("Orbit: engine " << engine << ", " << loop_count << " event loop(s), "
             << config_.worker_threads << " worker thread(s), " << cpus.size() << " CPU(s) available"
             << (config_.cpu_affinity ? ", event loops pinned" : ""));
    // Timers and on_start hooks run on the pool, now that it and the
    // listeners exist (#205).
    stop_hooks_ran_ = false;
    started_ = true;
    concurrency::ThreadPool* pool = thread_pool_.get();
    scheduler_->start([pool](std::function<void()> job) { pool->enqueue(std::move(job)); });
    if (!start_hooks_.empty()) {
        pool->enqueue([this] {
            for (auto& hook : start_hooks_) {
                try {
                    hook(*this);
                } catch (const std::exception& e) {
                    LOG_ERROR("on_start hook threw: " << e.what());
                } catch (...) {
                    LOG_ERROR("on_start hook threw");
                }
            }
        });
    }

    event_loops_.front()->run();
    // The other loops stop with the first (stop()) or drain on their own
    // (shutdown()); either way, wait for them.
    for (auto& t : loop_threads_) t.join();
    loop_threads_.clear();
    run_stop_hooks();
    scheduler_->stop();
    started_ = false;
}

App& App::on_start(std::function<void(App&)> hook) {
    start_hooks_.push_back(std::move(hook));
    return *this;
}

App& App::on_stop(std::function<void(App&)> hook) {
    stop_hooks_.push_back(std::move(hook));
    return *this;
}

TimerHandle App::run_every(std::chrono::milliseconds interval, std::function<void()> callback) {
    if (interval.count() <= 0) throw std::invalid_argument("run_every: the interval must be positive");
    return scheduler_->add(interval, interval, std::move(callback));
}

TimerHandle App::run_after(std::chrono::milliseconds delay, std::function<void()> callback) {
    return scheduler_->add(delay, std::chrono::milliseconds(0), std::move(callback));
}

void App::run_stop_hooks() {
    if (!started_ || stop_hooks_ran_.exchange(true)) return;
    for (auto& hook : stop_hooks_) {
        try {
            hook(*this);
        } catch (const std::exception& e) {
            LOG_ERROR("on_stop hook threw: " << e.what());
        } catch (...) {
            LOG_ERROR("on_stop hook threw");
        }
    }
}

std::vector<size_t> App::connections_per_event_loop() const {
    std::lock_guard<std::mutex> lock(loop_mutex_);
    std::vector<size_t> counts;
    for (const auto& loop : event_loops_) counts.push_back(loop->connection_count());
    return counts;
}

bool App::reload_tls(std::string* error) {
    if (!tls_context_) {
        if (error) *error = "TLS is not enabled";
        return false;
    }
    return tls_context_->reload(error);
}

void App::shutdown() {
    shutdown(config_.shutdown_timeout);
}

void App::shutdown(std::chrono::seconds timeout) {
    run_stop_hooks(); // before the drain, outside the lock (hooks may call back in)
    std::lock_guard<std::mutex> lock(loop_mutex_);
    draining_ = true;
    if (!event_loops_.empty()) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (auto& loop : event_loops_) loop->request_shutdown(deadline);
    } else {
        // Not started yet: nothing to drain.
        stop_requested_ = true;
    }
}

App& App::enable_health_checks(const std::string& liveness, const std::string& readiness) {
    this->get(liveness, [](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        http::HttpResponse response;
        response.set_body("ok", "text/plain");
        res->send(std::move(response));
    });
    this->get(readiness, [this](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> res) {
        http::HttpResponse response;
        if (draining_) {
            response.status(http::HttpStatus::ServiceUnavailable);
            response.set_body("draining", "text/plain");
        } else {
            response.set_body("ready", "text/plain");
        }
        res->send(std::move(response));
    });
    return *this;
}

void App::stop() {
    run_stop_hooks();
    std::lock_guard<std::mutex> lock(loop_mutex_);
    stop_requested_ = true;
    for (auto& loop : event_loops_) loop->stop();
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
        // Parent: the new process takes new connections (SO_REUSEPORT);
        // this one drains, bounded by shutdown_timeout.
        shutdown();
    } else {
        LOG_ERROR("Failed to fork for hot reload: " << strerror(errno));
    }
#else
    LOG_ERROR("Hot reload not supported on Windows");
#endif
}

} // namespace server
