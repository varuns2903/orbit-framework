#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/json.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::server { class App; }
namespace orbit::routing { class Router; }

namespace orbit::testing {

/**
 * @brief What the application answered, as a test sees it.
 */
struct Response {
    int status = 0;
    /// Header names as the application set them; look them up with header().
    std::map<std::string, std::string> headers;
    /// The whole body: the response body, or every chunk of a streamed one.
    std::string body;
    /// A streamed response's chunks (write_chunk() and SSE events), in order.
    std::vector<std::string> chunks;
    std::vector<http::Cookie> cookies; ///< Set-Cookie entries
    /// False if send()'s timeout passed first; a streamed response that has
    /// not called end() is incomplete, with what it sent so far.
    bool complete = false;

    /// A header, by case-insensitive name.
    std::optional<std::string> header(std::string_view name) const;
    /// The body parsed as JSON (null if it is not JSON).
    nlohmann::json json() const;
};

class Client;

/**
 * @brief One request, built fluently and run with send().
 */
class RequestBuilder {
public:
    RequestBuilder& header(std::string name, std::string value);
    /// The body and, unless @p content_type is empty, its Content-Type.
    RequestBuilder& body(std::string body, std::string content_type = "text/plain");
    /// A JSON body (Content-Type: application/json).
    RequestBuilder& json(const nlohmann::json& value);
    /// A cookie for this request only, on top of the client's cookie jar.
    RequestBuilder& cookie(std::string name, std::string value);
    /// The client address the application sees (default 127.0.0.1).
    RequestBuilder& client_ip(std::string ip);

    /**
     * @brief Runs the request through the application and waits until the
     *        response is complete, or @p timeout passes (a coroutine handler
     *        still waiting, an endless SSE stream). Never throws for an
     *        application error: that is a 500 (or what on_error answers).
     */
    Response send(std::chrono::milliseconds timeout = std::chrono::seconds(5));

private:
    friend class Client;
    RequestBuilder(Client& client, http::HttpMethod method, std::string target);

    Client& client_;
    http::HttpMethod method_;
    std::string target_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    std::unordered_map<std::string, std::string> cookies_;
    std::string client_ip_ = "127.0.0.1";
};

/**
 * @brief Calls an application's routes in-process, without listen() or
 *        sockets: for fast, port-free tests.
 *
 * Requests go through the same router as on the network: global, prefix and
 * group middleware, routing (404/405), error handlers, sessions. Responses
 * are collected in memory, chunks and SSE events included. Coroutine
 * handlers work: the client runs a small thread pool and event loop of its
 * own (ResponseWriter::thread_pool() / proactor()) and waits for the
 * response.
 *
 * @code
 * orbit::server::App app(orbit::config::ServerConfig{});
 * app.post("/tasks", create_task);
 * orbit::testing::Client client(app);
 * auto res = client.post("/tasks").json({{"title", "x"}}).send();
 * EXPECT_EQ(res.status, 201);
 * @endcode
 *
 * The client keeps a cookie jar: cookies the application sets are sent with
 * later requests, so a login followed by an authenticated request works.
 * What the network layer adds (Date, Content-Length framing, compression of
 * the wire format) is not applied.
 */
class Client {
public:
    explicit Client(server::App& app, size_t worker_threads = 2);
    explicit Client(const routing::Router& router, size_t worker_threads = 2);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    RequestBuilder get(std::string target) { return {*this, http::HttpMethod::GET, std::move(target)}; }
    RequestBuilder post(std::string target) { return {*this, http::HttpMethod::POST, std::move(target)}; }
    RequestBuilder put(std::string target) { return {*this, http::HttpMethod::PUT, std::move(target)}; }
    RequestBuilder patch(std::string target) { return {*this, http::HttpMethod::PATCH, std::move(target)}; }
    RequestBuilder del(std::string target); // out of line: windows.h may define DELETE as a macro
    RequestBuilder options(std::string target) { return {*this, http::HttpMethod::OPTIONS, std::move(target)}; }
    RequestBuilder head(std::string target) { return {*this, http::HttpMethod::HEAD, std::move(target)}; }
    RequestBuilder request(http::HttpMethod method, std::string target) { return {*this, method, std::move(target)}; }

    /// The cookie jar: name -> value, updated from every response.
    std::map<std::string, std::string>& cookies() { return cookies_; }

private:
    friend class RequestBuilder;
    struct Runtime; // thread pool and event loop for handlers

    const routing::Router& router_;
    std::unique_ptr<Runtime> runtime_;
    std::map<std::string, std::string> cookies_;
};

} // namespace orbit::testing
