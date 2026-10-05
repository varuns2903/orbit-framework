#include <orbit/http/Client.hpp>
#include <curl/curl.h>
// After curl.h: on Windows it pulls in <windows.h>, which #defines ERROR;
// Logger.hpp #undefs it again so LOG_ERROR expands correctly.
#include <orbit/utils/Logger.hpp>

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace http {

namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// One request in flight: owns everything libcurl points into.
struct Transfer {
    ClientRequest request;
    std::function<void(ClientResponse)> on_done;
    ClientResponse response;
    CURL* easy = nullptr;
    curl_slist* header_list = nullptr;
    bool too_large = false;
    char error_buffer[CURL_ERROR_SIZE] = {};

    ~Transfer() {
        if (header_list) curl_slist_free_all(header_list);
        if (easy) curl_easy_cleanup(easy);
    }
};

size_t on_body(char* data, size_t size, size_t n, void* user) {
    auto* t = static_cast<Transfer*>(user);
    size_t bytes = size * n;
    if (t->response.body.size() + bytes > t->request.max_response_size) {
        t->too_large = true;
        return 0; // aborts the transfer
    }
    t->response.body.append(data, bytes);
    return bytes;
}

size_t on_header(char* data, size_t size, size_t n, void* user) {
    auto* t = static_cast<Transfer*>(user);
    size_t bytes = size * n;
    std::string_view line(data, bytes);
    if (line.rfind("HTTP/", 0) == 0) {
        // A new response (after a redirect or a 100 Continue): only the
        // final response's headers are reported.
        t->response.headers.clear();
    } else if (size_t colon = line.find(':'); colon != std::string_view::npos && colon > 0) {
        t->response.headers.emplace_back(std::string(trim(line.substr(0, colon))),
                                         std::string(trim(line.substr(colon + 1))));
    }
    return bytes;
}

} // namespace

std::optional<std::string> ClientResponse::header(std::string_view name) const {
    for (auto it = headers.rbegin(); it != headers.rend(); ++it) {
        if (iequals(it->first, name)) return it->second;
    }
    return std::nullopt;
}

struct Client::Impl {
    ClientOptions options;
    CURLM* multi = nullptr;
    std::thread thread;
    std::thread::id thread_id;
    std::mutex mutex;
    std::vector<std::unique_ptr<Transfer>> incoming; // guarded by mutex
    std::atomic<bool> stopping{false};
    std::unordered_map<CURL*, std::unique_ptr<Transfer>> active; // client thread only

    void start(std::unique_ptr<Transfer> t) {
        const ClientRequest& r = t->request;
        CURL* easy = curl_easy_init();
        if (!easy) {
            finish(std::move(t), "curl_easy_init failed");
            return;
        }
        t->easy = easy;
        curl_easy_setopt(easy, CURLOPT_URL, r.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PRIVATE, t.get());
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, t->error_buffer);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, options.user_agent.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500 // 7.85.0
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(r.timeout.count()));
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(r.connect_timeout.count()));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, r.follow_redirects ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, r.max_redirects);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, r.verify_tls ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, r.verify_tls ? 2L : 0L);
        if (!r.ca_file.empty()) curl_easy_setopt(easy, CURLOPT_CAINFO, r.ca_file.c_str());
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, ""); // every coding libcurl can decode
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, t.get());
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, t.get());

        if (r.method == "HEAD") {
            curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        } else if (r.method != "GET") {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, r.method.c_str());
        }
        if (!r.body.empty() || r.method == "POST" || r.method == "PUT" || r.method == "PATCH") {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, r.body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        }
        // No "Expect: 100-continue" round trip before request bodies.
        t->header_list = curl_slist_append(t->header_list, "Expect:");
        for (const auto& [name, value] : r.headers) {
            std::string line = name + ": " + value;
            t->header_list = curl_slist_append(t->header_list, line.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, t->header_list);

        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            finish(std::move(t), "curl_multi_add_handle failed");
            return;
        }
        active.emplace(easy, std::move(t));
    }

    static void finish(std::unique_ptr<Transfer> t, std::string error) {
        t->response.error = std::move(error);
        if (!t->response.error.empty()) {
            t->response.body.clear();
            t->response.headers.clear();
        }
        auto on_done = std::move(t->on_done);
        ClientResponse response = std::move(t->response);
        t.reset(); // free libcurl resources before the callback
        if (on_done) on_done(std::move(response));
    }

    void complete(CURL* easy, CURLcode code) {
        auto it = active.find(easy);
        if (it == active.end()) return;
        std::unique_ptr<Transfer> t = std::move(it->second);
        active.erase(it);
        curl_multi_remove_handle(multi, easy);

        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &t->response.status);
        char* url = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url) t->response.effective_url = url;

        std::string error;
        if (t->too_large) {
            error = "response larger than max_response_size";
        } else if (code != CURLE_OK) {
            error = t->error_buffer[0] ? t->error_buffer : curl_easy_strerror(code);
        }
        finish(std::move(t), std::move(error));
    }

    void run() {
        thread_id = std::this_thread::get_id();
        while (true) {
            std::vector<std::unique_ptr<Transfer>> batch;
            {
                std::lock_guard<std::mutex> lock(mutex);
                batch.swap(incoming);
            }
            if (stopping) {
                for (auto& t : batch) finish(std::move(t), "client shut down");
                break;
            }
            for (auto& t : batch) start(std::move(t));

            int running = 0;
            curl_multi_perform(multi, &running);
            int queued = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &queued)) {
                if (msg->msg == CURLMSG_DONE) complete(msg->easy_handle, msg->data.result);
            }
            curl_multi_poll(multi, nullptr, 0, 1000, nullptr);
        }
        // Whatever is still running is abandoned.
        for (auto& [easy, t] : active) {
            curl_multi_remove_handle(multi, easy);
            finish(std::move(t), "client shut down");
        }
        active.clear();
    }
};

Client::Client(ClientOptions options) : impl_(std::make_unique<Impl>()) {
    static std::once_flag curl_init;
    std::call_once(curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->options = std::move(options);
    impl_->multi = curl_multi_init();
    if (!impl_->multi) throw std::runtime_error("curl_multi_init failed");
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

Client::~Client() {
    impl_->stopping = true;
    curl_multi_wakeup(impl_->multi);
    if (impl_->thread.joinable()) impl_->thread.join();
    {
        // send() calls that raced with shutdown
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& t : impl_->incoming) Impl::finish(std::move(t), "client shut down");
        impl_->incoming.clear();
    }
    curl_multi_cleanup(impl_->multi);
}

void Client::send(ClientRequest request, std::function<void(ClientResponse)> on_done) {
    auto t = std::make_unique<Transfer>();
    t->request = std::move(request);
    t->on_done = std::move(on_done);
    std::string_view url = t->request.url;
    if (!(url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
        Impl::finish(std::move(t), "only http:// and https:// URLs are supported");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->incoming.push_back(std::move(t));
    }
    curl_multi_wakeup(impl_->multi);
}

ClientResponse Client::send_sync(ClientRequest request) {
    if (std::this_thread::get_id() == impl_->thread_id) {
        ClientResponse r;
        r.error = "send_sync called on the client's own thread";
        return r;
    }
    // A condition variable rather than std::future: MSVC's <future> brings a
    // `concurrency` namespace that collides with Orbit's.
    struct Waiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        ClientResponse response;
    };
    auto waiter = std::make_shared<Waiter>();
    send(std::move(request), [waiter](ClientResponse r) {
        std::lock_guard<std::mutex> lock(waiter->mutex);
        waiter->response = std::move(r);
        waiter->done = true;
        waiter->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(waiter->mutex);
    waiter->cv.wait(lock, [&] { return waiter->done; });
    return std::move(waiter->response);
}

Client& Client::shared() {
    static Client client;
    return client;
}

} // namespace http
