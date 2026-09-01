#pragma once

#include <orbit/server/Listener.hpp>
#include <orbit/server/EventLoop.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/network/TlsContext.hpp>
#include <orbit/network/UdpSocket.hpp>
#ifdef ORBIT_ENABLE_HTTP3
#include <orbit/server/QuicConnectionManager.hpp>
#else
namespace server { class QuicConnectionManager; }
#endif
#include <memory>

namespace server {

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
    
    // WebSockets
    /**
     * @brief Registers a WebSocket route.
     * @param path The URL path.
     * @param handler The WebSocket handler function.
     * @return Reference to the App instance for chaining.
     */
    App& ws(const std::string& path, routing::WsHandler handler) {
        router_.ws(path, std::move(handler));
        return *this;
    }

    /**
     * @brief Retrieves the application's thread pool.
     * @return Reference to the ThreadPool.
     * @throws std::runtime_error If the server is not started.
     */
    concurrency::ThreadPool& get_thread_pool() {
        if (!event_loop_) throw std::runtime_error("Server not started");
        return event_loop_->get_thread_pool();
    }

    // Metrics
    /**
     * @brief Enables Prometheus metrics endpoint.
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
     * @return Reference to the App instance for chaining.
     */
    App& enable_openapi(const std::string& title = "Orbit Framework API", 
                        const std::string& version = "1.0.0", 
                        const std::string& docs_path = "/docs", 
                        const std::string& json_path = "/swagger.json");

    // Start the server (blocking)
    /**
     * @brief Starts the server and blocks the current thread.
     */
    void listen();
    
    // Stop the server gracefully
    /**
     * @brief Gracefully stops the server.
     */
    void stop();
    
    // Hot reload the server
    /**
     * @brief Triggers a hot reload of the server configuration and routes.
     */
    void hot_reload();

private:
    config::ServerConfig config_;
    routing::Router router_;
    std::unique_ptr<Listener> listener_;
#ifdef ORBIT_ENABLE_HTTP3
    std::unique_ptr<network::UdpSocket> quic_socket_;
    std::unique_ptr<QuicConnectionManager> quic_manager_;
#endif
    std::unique_ptr<network::TlsContext> tls_context_;
    std::unique_ptr<EventLoop> event_loop_;
};

} // namespace server
