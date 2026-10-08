#include <orbit/routing/Router.hpp>
#include <orbit/utils/Logger.hpp>
#include <filesystem>
#include <sstream>

namespace orbit::routing {

std::string Router::make_route_key(http::HttpMethod method, std::string_view path) const {
    std::string method_str;
    switch (method) {
        case http::HttpMethod::GET: method_str = "GET"; break;
        case http::HttpMethod::POST: method_str = "POST"; break;
        case http::HttpMethod::PUT: method_str = "PUT"; break;
        case http::HttpMethod::PATCH: method_str = "PATCH"; break;
        case http::HttpMethod::DELETE: method_str = "DELETE"; break;
        case http::HttpMethod::OPTIONS: method_str = "OPTIONS"; break;
        case http::HttpMethod::HEAD: method_str = "HEAD"; break;
        default: method_str = "UNKNOWN"; break;
    }
    return method_str + " " + std::string(path);
}

std::vector<std::string> Router::split_path(std::string_view path) const {
    std::vector<std::string> segments;
    std::string path_str(path);
    std::istringstream stream(path_str);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }
    return segments;
}

bool Router::has_ws_route(const std::string& path) const {
    return ws_routes_.find(path) != ws_routes_.end();
}

WsHandler Router::get_ws_route(const std::string& path) const {
    auto it = ws_routes_.find(path);
    if (it != ws_routes_.end()) {
        return it->second.handler;
    }
    return nullptr;
}

bool Router::run_ws_middlewares(const std::string& path, http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> response_writer) const {
    auto it = ws_routes_.find(path);
    if (it == ws_routes_.end()) return false;
    try {
        for (const auto& mw : middlewares_) {
            if (!mw(request, response_writer)) return false;
        }
        for (const auto& mw : it->second.middlewares) {
            if (!mw(request, response_writer)) return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Unhandled exception in WebSocket middleware for " << path << ": " << e.what());
        http::HttpResponse res;
        res.status(http::HttpStatus::InternalServerError).send("500 Internal Server Error");
        response_writer->send(std::move(res));
        return false;
    }
    return true;
}

void Router::group(const std::string& prefix, std::function<void(Router&)> callback) {
    // A group registers straight into the root router with the accumulated
    // prefix, and starts with the enclosing group's middleware. Pointing it
    // at the enclosing group instead lost nested routes, because that group
    // is a temporary.
    Router* root = this;
    while (root->parent_) root = root->parent_;
    Router group_router(prefix_ + prefix, root);
    group_router.local_middlewares_ = local_middlewares_;
    callback(group_router);
}

void Router::add_route(http::HttpMethod method, const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    openapi::RouteMetadata empty_meta;
    add_route_with_meta(method, path, std::move(mws), empty_meta, std::move(handler));
}

void Router::add_route_with_meta(http::HttpMethod method, const std::string& path, std::vector<Middleware> mws, const openapi::RouteMetadata& meta, RouteHandler handler) {
    std::string full_path = prefix_ + path;
    
    // Register to OpenAPI registry
    openapi_->register_route(method, full_path, meta);

    // Combine group-level middlewares with route-specific middlewares
    std::vector<Middleware> combined_mws = local_middlewares_;
    combined_mws.insert(combined_mws.end(), mws.begin(), mws.end());

    // Wrap handler with middlewares
    RouteHandler wrapped = [combined_mws, h = std::move(handler)](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
        for (const auto& mw : combined_mws) {
            if (!mw(req, writer)) {
                return; // Middleware aborted the request (e.g., sent an error response)
            }
        }
        h(req, writer);
    };

    if (full_path.find(':') != std::string::npos) {
        DynamicRoute dr;
        dr.method = method;
        dr.path_segments = split_path(full_path);
        dr.handler = std::move(wrapped);
        if (parent_) {
            parent_->dynamic_routes_.push_back(std::move(dr));
        } else {
            dynamic_routes_.push_back(std::move(dr));
        }
    } else {
        std::string key = make_route_key(method, full_path);
        if (parent_) {
            parent_->routes_[key] = std::move(wrapped);
        } else {
            routes_[key] = std::move(wrapped);
        }
    }
}

void Router::add_route(http::HttpMethod method, const std::string& path, RouteHandler handler) {
    add_route(method, path, {}, std::move(handler));
}

Router& Router::get(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::GET, path, std::move(handler));
    return *this;
}

Router& Router::get(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::GET, path, std::move(mws), std::move(handler));
    return *this;
}

Router& Router::post(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::POST, path, std::move(handler));
    return *this;
}

Router& Router::post(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::POST, path, std::move(mws), std::move(handler));
    return *this;
}

Router& Router::put(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::PUT, path, std::move(handler));
    return *this;
}

Router& Router::put(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::PUT, path, std::move(mws), std::move(handler));
    return *this;
}

Router& Router::patch(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::PATCH, path, std::move(handler));
    return *this;
}

Router& Router::patch(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::PATCH, path, std::move(mws), std::move(handler));
    return *this;
}

Router& Router::del(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::DELETE, path, std::move(handler));
    return *this;
}

Router& Router::del(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::DELETE, path, std::move(mws), std::move(handler));
    return *this;
}

Router& Router::options(const std::string& path, RouteHandler handler) {
    add_route(http::HttpMethod::OPTIONS, path, std::move(handler));
    return *this;
}

Router& Router::options(const std::string& path, std::vector<Middleware> mws, RouteHandler handler) {
    add_route(http::HttpMethod::OPTIONS, path, std::move(mws), std::move(handler));
    return *this;
}

void Router::add_stream_route(http::HttpMethod method, const std::string& path, RouteHandler handler) {
    Router* root = this;
    while (root->parent_) root = root->parent_;
    root->mark_stream_route(method, prefix_ + path);

    // Also register it as a normal route (with this group's middleware) so it gets executed
    add_route(method, path, std::move(handler));
}

void Router::mark_stream_route(http::HttpMethod method, const std::string& full_path) {
    if (full_path.find(':') != std::string::npos || full_path.find('*') != std::string::npos) {
        DynamicRoute dr;
        dr.method = method;
        dr.path_segments = split_path(full_path);
        dynamic_stream_routes_.push_back(std::move(dr));
    } else {
        std::string key = make_route_key(method, full_path);
        stream_routes_.insert(key);
    }
}

bool Router::is_stream_route(http::HttpMethod method, const std::string& path) const {
    if (parent_) {
        return parent_->is_stream_route(method, path);
    }
    
    std::string route_key = make_route_key(method, path);
    if (stream_routes_.find(route_key) != stream_routes_.end()) {
        return true;
    }
    
    auto req_segments = split_path(path);
    for (const auto& dr : dynamic_stream_routes_) {
        if (dr.method != method) continue;
        if (req_segments.size() != dr.path_segments.size()) continue;
        
        bool matches = true;
        for (size_t i = 0; i < dr.path_segments.size(); ++i) {
            if (dr.path_segments[i].empty()) continue;
            if (dr.path_segments[i][0] == ':') continue;
            if (dr.path_segments[i] != req_segments[i]) {
                matches = false;
                break;
            }
        }
        if (matches) return true;
    }
    
    return false;
}

void Router::ws(const std::string& path, WsHandler handler) {
    ws(path, {}, std::move(handler));
}

void Router::ws(const std::string& path, std::vector<Middleware> mws, WsHandler handler) {
    std::string full_path = prefix_ + path;
    // Group middleware wraps route middleware, exactly as for HTTP routes.
    std::vector<Middleware> combined = local_middlewares_;
    combined.insert(combined.end(), mws.begin(), mws.end());
    if (parent_) {
        parent_->ws(full_path, std::move(combined), std::move(handler));
    } else {
        ws_routes_[full_path] = WsRoute{std::move(handler), std::move(combined)};
    }
}

void Router::use(Middleware m) {
    if (parent_) {
        local_middlewares_.push_back(std::move(m));
    } else {
        middlewares_.push_back(std::move(m));
    }
}

void Router::on_error(ErrorHandler handler) {
    if (parent_) {
        parent_->on_error(std::move(handler));
    } else {
        error_handler_ = std::move(handler);
    }
}

void Router::route(http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> response_writer) const {
    try {
        // 1. Run global and route-specific middlewares
        for (auto& mw : middlewares_) {
            if (!mw(request, response_writer)) {
                return; // Pipeline stopped by middleware (e.g., auth failed, file served)
            }
        }
        
        // Finds the handler registered for `method` on this path: exact
        // routes first, then dynamic ones such as /users/:id.
        auto req_segments = split_path(request.uri);
        auto find_handler = [&](http::HttpMethod method, std::unordered_map<std::string, std::string>& params) -> const RouteHandler* {
            auto it = routes_.find(make_route_key(method, request.uri));
            if (it != routes_.end()) return &it->second;
            for (const auto& dr : dynamic_routes_) {
                if (dr.method != method || req_segments.size() != dr.path_segments.size()) continue;
                std::unordered_map<std::string, std::string> extracted;
                bool matches = true;
                for (size_t i = 0; i < dr.path_segments.size(); ++i) {
                    if (dr.path_segments[i][0] == ':') {
                        extracted[dr.path_segments[i].substr(1)] = req_segments[i];
                    } else if (dr.path_segments[i] != req_segments[i]) {
                        matches = false;
                        break;
                    }
                }
                if (matches) {
                    params = std::move(extracted);
                    return &dr.handler;
                }
            }
            return nullptr;
        };

        std::unordered_map<std::string, std::string> params;
        const RouteHandler* handler = find_handler(request.method, params);
        if (!handler && request.method == http::HttpMethod::HEAD) {
            // RFC 9110 section 9.3.2: HEAD is GET without the content; the
            // connection drops the body.
            handler = find_handler(http::HttpMethod::GET, params);
        }
        if (handler) {
            request.params = std::move(params);
            (*handler)(request, response_writer);
            return;
        }

        // The path exists under other methods: 405 with Allow (RFC 9110 section 15.5.6).
        std::string allow;
        static const http::HttpMethod kMethods[] = {
            http::HttpMethod::GET, http::HttpMethod::HEAD, http::HttpMethod::POST, http::HttpMethod::PUT,
            http::HttpMethod::PATCH, http::HttpMethod::DELETE, http::HttpMethod::OPTIONS
        };
        for (http::HttpMethod m : kMethods) {
            std::unordered_map<std::string, std::string> ignored;
            bool exists = find_handler(m, ignored) != nullptr ||
                          (m == http::HttpMethod::HEAD && find_handler(http::HttpMethod::GET, ignored) != nullptr);
            if (exists) {
                std::string key = make_route_key(m, "");
                if (!allow.empty()) allow += ", ";
                allow += key.substr(0, key.size() - 1); // "GET " -> "GET"
            }
        }
        http::HttpResponse res;
        if (!allow.empty()) {
            res.status(http::HttpStatus::MethodNotAllowed).send("405 Method Not Allowed");
            res.headers["Allow"] = allow;
        } else {
            res.status(http::HttpStatus::NotFound).send("404 Not Found");
        }
        response_writer->send(std::move(res));
        
    } catch (const std::exception& e) {
        const ErrorHandler* handler = error_handler_ ? &error_handler_
                                    : (parent_ && parent_->error_handler_) ? &parent_->error_handler_
                                    : nullptr;
        if (handler) {
            // An error handler that throws used to escape route(): the
            // worker caught it, but the client never got a response and
            // waited for a timeout.
            try {
                (*handler)(e, request, response_writer);
                return;
            } catch (const std::exception& inner) {
                LOG_ERROR("Error handler failed for route " << request.uri << ": " << inner.what()
                          << " (handling: " << e.what() << ")");
            } catch (...) {
                LOG_ERROR("Error handler failed for route " << request.uri << " (handling: " << e.what() << ")");
            }
            // Never a second response after part of one has gone out.
            if (response_writer->has_responded()) return;
        } else {
            // The exception text can contain SQL, file paths or secrets; it
            // goes to the log, never to the client.
            LOG_ERROR("Unhandled exception in route " << request.uri << ": " << e.what());
        }
        http::HttpResponse res;
        res.status(http::HttpStatus::InternalServerError).send("500 Internal Server Error");
        response_writer->send(std::move(res));
    } catch (...) {
        LOG_ERROR("Unknown unhandled exception in route " << request.uri);
        http::HttpResponse res;
        res.status(http::HttpStatus::InternalServerError).send("500 Internal Server Error");
        response_writer->send(std::move(res));
    }
}

void Router::RouteBuilder::handler(RouteHandler h) {
    router_.add_route_with_meta(method_, path_, std::move(mws_), meta_, std::move(h));
}

} // namespace routing
