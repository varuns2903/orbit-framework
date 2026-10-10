#include <orbit/testing/Client.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/http/Http2Session.hpp> // detail::apply_request_target
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/routing/Router.hpp>
#include <orbit/server/App.hpp>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <orbit/network/KqueueProactor.hpp>
#elif defined(_WIN32)
#include <orbit/network/IocpProactor.hpp>
#else
#include <orbit/network/EpollProactor.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#ifdef DELETE
#undef DELETE // windows.h, through the platform headers
#endif

namespace orbit::testing {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The request and the storage its string_views point into. Shared with the
// writer (and, through storage_owner, with coroutine handlers), so it lives
// as long as anything still answering it.
struct Exchange {
    http::HttpRequest request;
    std::string body;
    std::string cookie_header;
};

// Collects a response in memory. Thread-safe: coroutine handlers answer from
// whichever thread resumes them.
class TestWriter : public http::ResponseWriter {
public:
    TestWriter(network::Proactor& proactor, concurrency::ThreadPool& pool, bool head,
               std::shared_ptr<Exchange> exchange)
        : proactor_(proactor), pool_(pool), head_(head), exchange_(std::move(exchange)) {}

    void add_interceptor(Interceptor interceptor) override {
        std::lock_guard<std::mutex> lock(mutex_);
        interceptors_.push_back(std::move(interceptor));
    }
    void set_header(const std::string& key, const std::string& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        default_headers_[key] = value;
    }
    network::Proactor& proactor() override { return proactor_; }
    concurrency::ThreadPool& thread_pool() override { return pool_; }

    void send(http::HttpResponse&& response) override {
        mark_responded();
        prepare(response);
        std::lock_guard<std::mutex> lock(mutex_);
        if (result_.complete || streaming_) return; // one response per request
        take_head(response);
        if (!head_) result_.body = std::move(response.body);
        result_.complete = true;
        done_.notify_all();
    }
    void send_headers(http::HttpResponse& response) override {
        mark_responded();
        prepare(response);
        std::lock_guard<std::mutex> lock(mutex_);
        if (result_.complete || streaming_) return;
        take_head(response);
        streaming_ = true;
    }
    void write_chunk(std::string_view chunk) override {
        mark_responded();
        std::lock_guard<std::mutex> lock(mutex_);
        if (result_.complete || head_) return;
        result_.chunks.emplace_back(chunk);
        result_.body.append(chunk);
        done_.notify_all();
    }
    void end() override {
        std::lock_guard<std::mutex> lock(mutex_);
        result_.complete = true;
        done_.notify_all();
    }
    void send_sse_event(std::string_view data, std::string_view event, std::string_view id) override {
        // As on the wire (Connection::send_sse_event).
        std::string msg;
        if (!event.empty()) msg += "event: " + std::string(event) + "\n";
        if (!id.empty()) msg += "id: " + std::string(id) + "\n";
        size_t start = 0;
        while (start < data.size()) {
            size_t nl = data.find('\n', start);
            msg += "data: " + std::string(data.substr(start, nl == std::string_view::npos ? nl : nl - start)) + "\n";
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
        msg += "\n";
        write_chunk(msg);
    }
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {
        throw std::logic_error("orbit::testing::Client does not support raw streams or WebSocket upgrades");
    }
    void read_body_stream(std::function<void(std::string_view)> on_data, std::function<void()> on_end) override {
        // The whole body is already here: one piece, then the end.
        if (!exchange_->body.empty() && on_data) on_data(exchange_->body);
        if (on_end) on_end();
    }

    Response wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait_for(lock, timeout, [this] { return result_.complete; });
        return result_;
    }

    /// The test is done with this request: the "client" leaves.
    void close() { mark_closed(); }

private:
    // Interceptors, then default headers the response does not set itself,
    // as the server's connection applies them.
    void prepare(http::HttpResponse& response) {
        std::vector<Interceptor> interceptors;
        std::unordered_map<std::string, std::string> defaults;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interceptors = interceptors_;
            defaults = default_headers_;
        }
        for (auto& interceptor : interceptors) interceptor(response);
        for (const auto& [k, v] : defaults) {
            if (response.headers.find(k) == response.headers.end()) response.headers[k] = v;
        }
    }
    void take_head(const http::HttpResponse& response) {
        result_.status = static_cast<int>(response.status_code);
        for (const auto& [k, v] : response.headers) result_.headers[k] = v;
        result_.cookies = response.cookies;
    }

    network::Proactor& proactor_;
    concurrency::ThreadPool& pool_;
    const bool head_;
    std::shared_ptr<Exchange> exchange_;

    std::mutex mutex_;
    std::condition_variable done_;
    std::vector<Interceptor> interceptors_;
    std::unordered_map<std::string, std::string> default_headers_;
    bool streaming_ = false;
    Response result_;
};

} // namespace

// --- Response ---

std::optional<std::string> Response::header(std::string_view name) const {
    for (const auto& [k, v] : headers) {
        if (iequals(k, name)) return v;
    }
    return std::nullopt;
}

nlohmann::json Response::json() const {
    return nlohmann::json::parse(body, nullptr, false);
}

// --- Client ---

struct Client::Runtime {
#if defined(__APPLE__) || defined(__FreeBSD__)
    network::KqueueProactor proactor;
#elif defined(_WIN32)
    network::IocpProactor proactor;
#else
    network::EpollProactor proactor;
#endif
    concurrency::ThreadPool pool;
    std::atomic<bool> running{true};
    std::thread loop;

    explicit Runtime(size_t threads) : pool(threads == 0 ? 1 : threads) {
        loop = std::thread([this] {
            while (running.load()) proactor.run_once(20);
        });
    }
    ~Runtime() {
        running = false;
        if (loop.joinable()) loop.join();
    }
};

Client::Client(server::App& app, size_t worker_threads) : Client(app.router(), worker_threads) {}

Client::Client(const routing::Router& router, size_t worker_threads)
    : router_(router), runtime_(std::make_unique<Runtime>(worker_threads)) {}

Client::~Client() = default;

RequestBuilder Client::del(std::string target) {
    return {*this, http::HttpMethod::DELETE, std::move(target)};
}

// --- RequestBuilder ---

RequestBuilder::RequestBuilder(Client& client, http::HttpMethod method, std::string target)
    : client_(client), method_(method), target_(std::move(target)) {}

RequestBuilder& RequestBuilder::header(std::string name, std::string value) {
    headers_.emplace_back(std::move(name), std::move(value));
    return *this;
}

RequestBuilder& RequestBuilder::body(std::string body, std::string content_type) {
    body_ = std::move(body);
    if (!content_type.empty()) header("Content-Type", std::move(content_type));
    return *this;
}

RequestBuilder& RequestBuilder::json(const nlohmann::json& value) {
    return body(value.dump(), "application/json");
}

RequestBuilder& RequestBuilder::cookie(std::string name, std::string value) {
    cookies_[std::move(name)] = std::move(value);
    return *this;
}

RequestBuilder& RequestBuilder::client_ip(std::string ip) {
    client_ip_ = std::move(ip);
    return *this;
}

Response RequestBuilder::send(std::chrono::milliseconds timeout) {
    auto exchange = std::make_shared<Exchange>();
    http::HttpRequest& req = exchange->request;
    req.storage_owner = exchange;
    req.method = method_;
    req.http_version = "HTTP/1.1";
    req.client_ip = client_ip_;
    req.peer_ip = client_ip_;

    if (!http::h2::detail::apply_request_target(target_, req)) {
        Response bad;
        bad.status = 400;
        bad.body = "400 Bad Request";
        bad.complete = true;
        return bad;
    }

    bool has_host = false;
    for (auto& [name, value] : headers_) {
        if (iequals(name, "Host")) has_host = true;
        req.set_header(name, value);
    }
    if (!has_host) req.set_header("Host", "localhost");

    // The jar, then this request's own cookies on top.
    std::map<std::string, std::string> cookies = client_.cookies_;
    for (auto& [name, value] : cookies_) cookies[name] = value;
    for (const auto& [name, value] : cookies) {
        if (!exchange->cookie_header.empty()) exchange->cookie_header += "; ";
        exchange->cookie_header += name + "=" + value;
        req.cookies[name] = value;
    }
    if (!exchange->cookie_header.empty()) req.set_header("Cookie", exchange->cookie_header);

    exchange->body = std::move(body_);
    req.body = exchange->body;
    if (!exchange->body.empty()) req.set_header("Content-Length", std::to_string(exchange->body.size()));

    auto writer = std::make_shared<TestWriter>(client_.runtime_->proactor, client_.runtime_->pool,
                                               method_ == http::HttpMethod::HEAD, exchange);
    client_.router_.route(req, writer);
    Response res = writer->wait(timeout);
    writer->close();

    for (const auto& c : res.cookies) {
        if (c.max_age == 0) {
            client_.cookies_.erase(c.name);
        } else {
            client_.cookies_[c.name] = c.value;
        }
    }
    return res;
}

} // namespace orbit::testing
