#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <vector>
#include <thread>

#include <orbit/server/Listener.hpp>
#include <orbit/server/EventLoop.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/routing/HandlerWrapper.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/network/TlsContext.hpp>
#include <orbit/network/UdpSocket.hpp>
#ifdef ORBIT_ENABLE_HTTP3
#include <orbit/server/QuicConnectionManager.hpp>
#else
namespace orbit::server { class QuicConnectionManager; }
#endif
#include <atomic>
#include <memory>
#include <mutex>

namespace orbit::server {

/**
 * @brief The main application class for the Orbit Framework.
 * 
 * The App class acts as the central orchestrator for the web framework. It manages the server's lifecycle,
 * routing, middlewares, dependency injection, and worker thread pools. 
 * Users instantiate this class, define their routes, and call `listen()` to start accepting connections.
 */
class App {
public:
    /**
     * @brief Constructs a new App instance.
     * @param config The server configuration containing port, worker threads, and event engine preferences.
     */
    App(const config::ServerConfig& config);
    ~App();

    // Delete copy constructors
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Middleware
    /**
     * @brief Registers a global error handler for the application.
     * @param handler The error handler function.
     */
    void on_error(routing::ErrorHandler handler);

    /**
     * @brief Registers a global middleware.
     * @param m The middleware function.
     * @return Reference to the App instance for chaining.
     */
    App& use(routing::Middleware m);

    /**
     * @brief Runs @p m only for requests under @p prefix ("/api" and "/api/...").
     * @details Runs before route matching, so it can handle paths without a
     *          route, e.g. `app.use("/api", orbit::middleware::proxy(opts))`.
     */
    App& use(const std::string& prefix, routing::Middleware m);

    /// Answers requests that match no route instead of "404 Not Found".
    App& not_found(routing::RouteHandler handler);

    // Route Grouping
    /**
     * @brief Creates a route group with a specific prefix.
     * @param prefix The URL prefix for the group.
     * @param callback A function to configure routes within the group.
     * @return Reference to the App instance for chaining.
     */
    App& group(const std::string& prefix, std::function<void(routing::Router&)> callback) {
        router_.group(prefix, std::move(callback));
        return *this;
    }

    /**
     * @brief Initiates a route builder for a specific path and method.
     * @param path The URL path.
     * @param method The HTTP method (default is GET).
     * @return A RouteBuilder instance to configure the route.
     */
    routing::Router::RouteBuilder route(const std::string& path, http::HttpMethod method = http::HttpMethod::GET) {
        return router_.route(path, method);
    }

    // Fluent routing API
    /**
     * @brief Registers a GET route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& get(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers a GET route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& get(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
    /**
     * @brief Registers a POST route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& post(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers a POST route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& post(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
    /**
     * @brief Registers a PUT route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& put(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers a PUT route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& put(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
    /**
     * @brief Registers a PATCH route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& patch(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers a PATCH route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& patch(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
    /**
     * @brief Registers a DELETE route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& del(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers a DELETE route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& del(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
    /**
     * @brief Registers an OPTIONS route.
     * @param path The URL path.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& options(const std::string& path, routing::RouteHandler handler);
    /**
     * @brief Registers an OPTIONS route with middleware.
     * @param path The URL path.
     * @param mws A vector of middlewares to apply.
     * @param handler The route handler function.
     * @return Reference to the App instance for chaining.
     */
    App& options(const std::string& path, std::vector<routing::Middleware> mws, routing::RouteHandler handler);
    
#define ORBIT_DEFINE_ROUTER_TEMPLATES(METHOD) \
    template <typename Handler> \
    App& METHOD(const std::string& path, Handler&& handler) { \
        return METHOD(path, routing::wrap_handler(std::forward<Handler>(handler))); \
    } \
    template <typename Handler> \
    App& METHOD(const std::string& path, std::vector<routing::Middleware> mws, Handler&& handler) { \
        return METHOD(path, std::move(mws), routing::wrap_handler(std::forward<Handler>(handler))); \
    }

    ORBIT_DEFINE_ROUTER_TEMPLATES(get)
    ORBIT_DEFINE_ROUTER_TEMPLATES(post)
    ORBIT_DEFINE_ROUTER_TEMPLATES(put)
    ORBIT_DEFINE_ROUTER_TEMPLATES(patch)
    ORBIT_DEFINE_ROUTER_TEMPLATES(del)
    ORBIT_DEFINE_ROUTER_TEMPLATES(options)
#undef ORBIT_DEFINE_ROUTER_TEMPLATES

    // WebSockets
    /**
     * @brief Registers a WebSocket route.
     * @param path The URL path.
     * @param handler The WebSocket handler function.
     * @return Reference to the App instance for chaining.
     */
    App& ws(const std::string& path, std::vector<routing::Middleware> mws, routing::WsHandler handler) {
        router_.ws(path, std::move(mws), std::move(handler));
        return *this;
    }

    App& ws(const std::string& path, routing::WsHandler handler) {
        router_.ws(path, std::move(handler));
        return *this;
    }

    /**
     * @brief Retrieves the application's thread pool.
     * @return Reference to the ThreadPool.
     * @throws std::runtime_error If the server is not started.
     */
    /**
     * @brief Open connections on each event loop (one entry per loop; empty
     *        before listen()). See ServerConfig::event_loops.
     */
    std::vector<size_t> connections_per_event_loop() const;

    /**
     * @brief The configuration in effect: once listen() has started, "auto"
     *        settings (event_loops = 0, worker_threads = 0, EventEngine::Auto)
     *        show what they resolved to.
     */
    const config::ServerConfig& effective_config() const { return config_; }

    concurrency::ThreadPool& get_thread_pool() {
        std::lock_guard<std::mutex> lock(loop_mutex_);
        if (!thread_pool_) throw std::runtime_error("Server not started");
        return *thread_pool_;
    }

    /**
     * @brief This app's OpenAPI registry: its routes, plus schemas added with
     *        openapi().register_schema(). Each App has its own, so two Apps
     *        in one process publish separate specs.
     */
    openapi::OpenApiRegistry& openapi() { return router_.openapi(); }

    // Metrics
    /**
     * @brief Enables Prometheus metrics endpoint.
     *
     * Metrics are process-wide, as in other Prometheus clients: with several
     * Apps in one process, each endpoint reports the same totals.
     * @param path The URL path for metrics (default is "/metrics").
     * @return Reference to the App instance for chaining.
     */
    App& enable_metrics(const std::string& path = "/metrics");

    // OpenAPI & Swagger UI
    /**
     * @brief Enables OpenAPI documentation and Swagger UI.
     * @param title The API title.
     * @param version The API version.
     * @param docs_path The URL path for Swagger UI.
     * @param json_path The URL path for the OpenAPI JSON.
     * @param assets_url Where the Swagger UI files (swagger-ui.css and
     *        swagger-ui-bundle.js from swagger-ui-dist 5.11.0) are loaded
     *        from. Defaults to the unpkg CDN; point it at your own copy
     *        (e.g. "/swagger-ui" served by static_files) to keep the docs
     *        page off third-party origins. Either way the page pins the
     *        files with Subresource Integrity hashes, so modified files are
     *        refused by the browser.
     * @return Reference to the App instance for chaining.
     *
     * The docs page is only registered when this is called; enable it only
     * in environments where the API description may be public.
     */
    App& enable_openapi(const std::string& title = "Orbit Framework API", 
                        const std::string& version = "1.0.0", 
                        const std::string& docs_path = "/docs", 
                        const std::string& json_path = "/swagger.json",
                        const std::string& assets_url = "https://unpkg.com/swagger-ui-dist@5.11.0");

    // Start the server (blocking)
    /**
     * @brief Starts the server and blocks the current thread.
     */
    void listen();
    
    // Stop the server gracefully
    /**
     * @brief Stops the server immediately: open connections are dropped.
     *        Safe to call from any thread, even before listen().
     */
    void stop();

    /**
     * @brief Stops the server gracefully (what SIGTERM/SIGINT do): stop
     *        accepting, mark the app not ready, let in-flight requests finish,
     *        close idle connections, and force-close the rest after `timeout`
     *        (default: ServerConfig::shutdown_timeout). listen() returns when
     *        done. Safe to call from any thread.
     */
    void shutdown();

    /**
     * @brief Re-reads the TLS certificate and key files (ssl_cert, ssl_key and
     *        sni_certificates). Thread-safe; also triggered by SIGHUP and by
     *        ServerConfig::tls_reload_interval.
     *
     * New handshakes use the new certificates; open connections are not
     * affected. If any file fails to load, the current certificates stay.
     * @return False if TLS is off or loading failed (see @p error).
     */
    bool reload_tls(std::string* error = nullptr);
    void shutdown(std::chrono::seconds timeout);

    /**
     * @brief Adds health endpoints for orchestrators such as Kubernetes:
     *        `liveness` answers 200 while the process runs; `readiness`
     *        answers 200 while serving and 503 once shutdown has begun, so
     *        load balancers stop sending new traffic.
     */
    App& enable_health_checks(const std::string& liveness = "/healthz", const std::string& readiness = "/readyz");

    /// True once shutdown() (or SIGTERM/SIGINT) has started draining.
    bool is_draining() const { return draining_.load(); }
    
    // Hot reload the server
    /**
     * @brief Triggers a hot reload of the server configuration and routes.
     */
    void hot_reload();

private:
    config::ServerConfig config_;
    routing::Router router_;
    // One listening socket per event loop, all on the same port (SO_REUSEPORT).
    // Declared before the loops, which refer to them.
    std::vector<std::unique_ptr<Listener>> listeners_;
#ifdef ORBIT_ENABLE_HTTP3
    std::unique_ptr<network::UdpSocket> quic_socket_;
    std::unique_ptr<QuicConnectionManager> quic_manager_;
#endif
    std::unique_ptr<network::TlsContext> tls_context_;
    // event_loops_[0] runs on the thread that called listen() and handles
    // signals, TLS reloads and QUIC; the others run on loop_threads_.
    // max_connections budget shared by the loops when there are several.
    // Declared before the loops, which refer to it.
    std::atomic<size_t> open_connections_{0};
    std::vector<std::unique_ptr<EventLoop>> event_loops_;
    std::vector<std::thread> loop_threads_;
    // Shared by every loop. Declared after the loops so it is destroyed, and
    // its workers joined, before them: queued handlers hold connections that
    // use a loop's proactor.
    std::unique_ptr<concurrency::ThreadPool> thread_pool_;
    // listen() creates the loops on the server thread while stop() may run
    // on any thread (a test, a signal, an admin endpoint).
    mutable std::mutex loop_mutex_;
    bool stop_requested_ = false; // guarded by loop_mutex_; a stop before the loop exists still applies
    unsigned seen_signal_seq_ = 0; // loop thread only
    std::chrono::steady_clock::time_point next_tls_check_{}; // loop thread only
    std::atomic<bool> draining_{false};
};

} // namespace server
