#pragma once
#include <orbit/routing/Router.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace middleware {

/**
 * @ingroup middlewares
 * @brief Options for Proxy middleware.
 */
struct ProxyOptions {
    std::string target_host;
    int target_port;
    std::string strip_prefix = "";
    bool use_tls = false;                  ///< Connect to the upstream over TLS (also implied by port 443)
    bool verify_tls = true;                ///< Verify the upstream certificate chain and hostname
    std::string ca_file;                   ///< Optional PEM CA bundle for upstream verification
    bool trust_forwarded_headers = false;  ///< Keep and extend client-supplied X-Forwarded-* headers
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that proxies requests to a single target URL (ip/hostname and port).
 *
 * @param options The proxy configuration options.
 * @return routing::Middleware The proxy middleware handler.
 */
routing::Middleware proxy(ProxyOptions options);

/**
 * @ingroup middlewares
 * @brief Returns a middleware that proxies requests to a single target URL.
 *
 * @param target_host The target host.
 * @param target_port The target port.
 * @return routing::Middleware The proxy middleware handler.
 */
routing::Middleware proxy(const std::string& target_host, int target_port);

/**
 * @ingroup middlewares
 * @brief A target node for the load balancer.
 */
struct TargetNode {
    std::string host;
    int port;
    bool use_tls = false; ///< Connect over TLS (also implied by port 443)
};

/**
 * @ingroup middlewares
 * @brief Options for LoadBalancer middleware.
 */
struct LoadBalancerOptions {
    std::vector<TargetNode> nodes;
    std::string strip_prefix = "";
    bool verify_tls = true;
    std::string ca_file;
    bool trust_forwarded_headers = false;
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that load-balances requests across multiple upstream nodes.
 *
 * Currently implements a Round-Robin algorithm.
 *
 * @param options The load balancer options.
 * @return routing::Middleware The load balancer middleware handler.
 */
routing::Middleware load_balancer(LoadBalancerOptions options);

/**
 * @ingroup middlewares
 * @brief Returns a middleware that load-balances requests across multiple upstream nodes.
 *
 * @param nodes A vector of target nodes.
 * @return routing::Middleware The load balancer middleware handler.
 */
routing::Middleware load_balancer(const std::vector<TargetNode>& nodes);

namespace detail {

/**
 * @brief An owned copy of the parts of a request the proxy forwards. The
 *        original HttpRequest, and the connection buffer its headers point
 *        into, are gone long before the asynchronous upstream connect completes.
 */
struct ProxiedRequest {
    std::string method;
    std::string target;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string client_ip;
};

ProxiedRequest snapshot_request(const http::HttpRequest& request);

/**
 * @brief Serializes the request sent upstream. Hop-by-hop headers are dropped
 *        case-insensitively, Host and Content-Length are set by the proxy, and
 *        client-supplied forwarding headers are replaced unless
 *        @p trust_forwarded_headers is true, in which case they are extended.
 */
std::string build_upstream_request(const ProxiedRequest& request, const std::string& host, int port,
                                   const std::string& strip_prefix, bool trust_forwarded_headers);

/**
 * @brief Incremental decoder for a chunked upstream response body.
 */
class ChunkedDecoder {
public:
    enum class Result { NeedMore, Done, Error };

    /// Decodes as much of @p data as possible, appending body bytes to @p out.
    Result feed(std::string_view data, std::string& out);

private:
    enum class State { Size, Data, DataEnd, Trailer, Finished };
    State state_{State::Size};
    std::string line_;
    size_t remaining_{0};
};

} // namespace detail

} // namespace middleware
