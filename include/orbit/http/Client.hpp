#pragma once
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http {

/// An outbound HTTP request for Client.
struct ClientRequest {
    std::string method = "GET";
    std::string url; ///< http:// or https:// only
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    std::chrono::milliseconds timeout{30000};         ///< Whole request, redirects included
    std::chrono::milliseconds connect_timeout{10000};
    bool follow_redirects = true;                      ///< To http(s) URLs only
    long max_redirects = 5;
    bool verify_tls = true;                            ///< Certificate chain and host name
    std::string ca_file;                               ///< PEM bundle; empty = system store
    size_t max_response_size = 10 * 1024 * 1024;       ///< Larger bodies fail the request
};

/// The result of a ClientRequest.
struct ClientResponse {
    long status = 0;                                   ///< 0 if no response arrived
    std::vector<std::pair<std::string, std::string>> headers; ///< Of the final response
    std::string body;
    std::string effective_url;                         ///< After redirects
    std::string error;                                 ///< Empty on success

    /// True if a response arrived (any status, including 4xx and 5xx).
    bool ok() const { return error.empty(); }
    /// The last header named @p name (case-insensitive).
    std::optional<std::string> header(std::string_view name) const;
};

struct ClientOptions {
    std::string user_agent = "Orbit-Framework";
};

class Client;

/// co_await Client::send_async(...): resumes with the ClientResponse.
struct ClientSendAwaiter {
    Client* client;
    ClientRequest request;
    ClientResponse response;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h);
    ClientResponse await_resume() { return std::move(response); }
};

/**
 * @brief Asynchronous HTTP/1.1 and HTTP/2 client (libcurl's multi interface).
 *
 * One background thread drives every transfer, so many requests run at
 * once without tying up worker threads, and connections are kept alive and
 * reused between requests to the same host. Only http and https URLs are
 * fetched, including on redirects, and TLS certificates are verified unless
 * a request turns that off.
 *
 * Completion callbacks, and coroutines resumed by send_async(), run on the
 * client's thread: keep them short, and hand blocking work to a thread pool.
 *
 * @code
 * http::ClientRequest req;
 * req.url = "https://api.example.com/items";
 * auto res = co_await http::Client::shared().send_async(req);
 * if (res.ok() && res.status == 200) { ... }
 * @endcode
 */
class Client {
public:
    explicit Client(ClientOptions options = {});
    /// Requests still in flight complete with error "client shut down".
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Starts @p request; @p on_done is called once, on the client's thread.
    void send(ClientRequest request, std::function<void(ClientResponse)> on_done);

    /// Awaitable form of send(); the coroutine resumes on the client's thread.
    ClientSendAwaiter send_async(ClientRequest request) { return ClientSendAwaiter{this, std::move(request), {}}; }

    /**
     * @brief Sends @p request and waits for the response, blocking the
     *        calling thread (for code that cannot be asynchronous).
     *
     * Called from the client's own thread (inside a completion callback) it
     * would wait forever, so it fails at once instead.
     */
    ClientResponse send_sync(ClientRequest request);

    /// A process-wide client, created on first use.
    static Client& shared();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline void ClientSendAwaiter::await_suspend(std::coroutine_handle<> h) {
    client->send(std::move(request), [this, h](ClientResponse r) {
        response = std::move(r);
        h.resume();
    });
}

} // namespace http
