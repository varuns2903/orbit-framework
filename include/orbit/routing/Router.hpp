#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/openapi/OpenApi.hpp>
#include <functional>
#include <exception>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <memory>

namespace orbit::routing {

using RouteHandler = std::function<void(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)>;
using Middleware = std::function<bool(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)>;
using WsHandler = std::function<void(http::websocket::WebSocketConnection&)>;
using ErrorHandler = std::function<void(const std::exception&, http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)>;

struct WsRoute {
    WsHandler handler;
    std::vector<Middleware> middlewares; // Group and route middleware, outermost first
};

struct DynamicRoute {
    http::HttpMethod method;
    std::vector<std::string> path_segments;
    RouteHandler handler;
};

/**
 * @brief Manages routing of HTTP requests to their appropriate handlers.
 */
class Router {
public:
    /**
     * @brief Constructs a new Router.
     * @param prefix The route prefix for this router.
     * @param parent Pointer to the parent router, if any.
     */
    Router(const std::string& prefix = "", Router* parent = nullptr) 
        : prefix_(prefix), parent_(parent),
          openapi_(parent ? parent->openapi_ : std::make_shared<openapi::OpenApiRegistry>()) {}

    /// The OpenAPI registry for this router tree (shared by its groups).
    openapi::OpenApiRegistry& openapi() { return *openapi_; }

    /**
     * @brief A builder class for fluent route configuration.
     */
    class RouteBuilder {
    public:
        RouteBuilder(Router& router, http::HttpMethod method, const std::string& path, std::vector<Middleware> mws = {})
            : router_(router), method_(method), path_(path), mws_(std::move(mws)) {}
        
        RouteBuilder& summary(const std::string& s) { meta_.summary = s; return *this; }
        RouteBuilder& description(const std::string& s) { meta_.description = s; return *this; }
        RouteBuilder& tag(const std::string& t) { meta_.tags.push_back(t); return *this; }
        RouteBuilder& req_body(const std::string& schema_name) { meta_.request_body_schema = schema_name; return *this; }
        RouteBuilder& res_body(int status, const std::string& schema_name) { meta_.response_schemas[status] = schema_name; return *this; }
        
        void handler(RouteHandler h);
    private:
        Router& router_;
        http::HttpMethod method_;
        std::string path_;
        std::vector<Middleware> mws_;
        openapi::RouteMetadata meta_;
    };

    /**
     * @brief Initiates a route builder for a specific path and method.
     * @param path The URL path.
     * @param method The HTTP method (default is GET).
     * @return A RouteBuilder instance to configure the route.
     * 
     * @code
     * router.route("/users", HttpMethod::POST)
     *       .summary("Create a user")
     *       .handler([](auto& req, auto res) { ... });
     * @endcode
     */
    RouteBuilder route(const std::string& path, http::HttpMethod method = http::HttpMethod::GET) {
        return RouteBuilder(*this, method, path);
    }

    // Route Grouping
    /**
     * @brief Creates a route group with a specific prefix.
     * @param prefix The URL prefix for the group.
     * @param callback A function to configure routes within the group.
     */
    void group(const std::string& prefix, std::function<void(Router&)> callback);

    // Register a handler for a specific HTTP method and path
    void add_route(http::HttpMethod method, const std::string& path, RouteHandler handler);
    void add_route(http::HttpMethod method, const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    void add_route_with_meta(http::HttpMethod method, const std::string& path, std::vector<Middleware> mws, const openapi::RouteMetadata& meta, RouteHandler handler);
    
    // Fluent routing API
    Router& get(const std::string& path, RouteHandler handler);
    Router& get(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    Router& post(const std::string& path, RouteHandler handler);
    Router& post(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    Router& put(const std::string& path, RouteHandler handler);
    Router& put(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    Router& patch(const std::string& path, RouteHandler handler);
    Router& patch(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    Router& del(const std::string& path, RouteHandler handler);
    Router& del(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    Router& options(const std::string& path, RouteHandler handler);
    Router& options(const std::string& path, std::vector<Middleware> mws, RouteHandler handler);
    
    void ws(const std::string& path, WsHandler handler);
    void ws(const std::string& path, std::vector<Middleware> mws, WsHandler handler);

    /**
     * @brief Runs global, group and route middleware for a WebSocket upgrade.
     * @return True if every middleware let the request through; false if one
     *         stopped it (it has then already sent a response).
     */
    bool run_ws_middlewares(const std::string& path, http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> response_writer) const;
    void use(Middleware m);

    /**
     * @brief Runs @p m only for requests under @p prefix: the prefix itself
     *        and anything below it ("/svc" and "/svc/..."; not "/svcx").
     *
     * Runs before route matching, so it can serve paths that have no route,
     * e.g. `app.use("/api", orbit::middleware::proxy(opts))`.
     * @throws std::invalid_argument if @p prefix does not start with '/'.
     */
    void use(const std::string& prefix, Middleware m);

    /// Answers requests that match no route (instead of "404 Not Found").
    /// A path that exists under other methods still gets 405 with Allow.
    void not_found(RouteHandler handler);
    
    // Register global error handler
    void on_error(ErrorHandler handler);

    // Route an incoming request to the correct handler
    void route(http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> response_writer) const;

    bool has_ws_route(const std::string& path) const;

private:
    // Error handling for an exception from a route, middleware or (later,
    // through the writer's error sink) an asynchronous handler.
    void handle_exception(std::exception_ptr error, http::HttpRequest& request,
                          std::shared_ptr<http::ResponseWriter> response_writer) const;

public:
    WsHandler get_ws_route(const std::string& path) const;

    // Check if a given path is registered as a streaming route
    bool is_stream_route(http::HttpMethod method, const std::string& path) const;
    void add_stream_route(http::HttpMethod method, const std::string& path, RouteHandler handler);

private:

    std::string make_route_key(http::HttpMethod method, std::string_view path) const;
    void mark_stream_route(http::HttpMethod method, const std::string& full_path);
    std::vector<std::string> split_path(std::string_view path) const;

    // Matches request segments against a route pattern of literal,
    // ":param" and a trailing "*" / "*name" segment. Fills @p params and
    // returns the number of literal segments matched, or -1 for no match.
    static int match_segments(const std::vector<std::string>& pattern, const std::vector<std::string>& request,
                              std::unordered_map<std::string, std::string>* params);
    static bool is_wildcard(const std::string& segment) { return !segment.empty() && segment[0] == '*'; }
    
    std::string prefix_;
    Router* parent_{nullptr};
    std::shared_ptr<openapi::OpenApiRegistry> openapi_;
    std::vector<Middleware> local_middlewares_;
    ErrorHandler error_handler_;
    RouteHandler not_found_handler_;
    
    std::unordered_map<std::string, RouteHandler> routes_;
    std::unordered_map<std::string, WsRoute> ws_routes_;
    std::vector<DynamicRoute> dynamic_routes_;
    std::vector<Middleware> middlewares_;
    
    std::unordered_set<std::string> stream_routes_;
    std::vector<DynamicRoute> dynamic_stream_routes_;
};

} // namespace routing
