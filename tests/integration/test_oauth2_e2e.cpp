#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/middleware/OAuth2.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "../utils/TestConfig.hpp"

using namespace http;

namespace {

constexpr uint16_t kProviderPort = 8108;
std::mutex g_mutex;
std::string g_last_token_body;

class CaptureWriter : public ResponseWriter {
public:
    HttpResponse last;
    std::vector<Interceptor> interceptors;
    void send(HttpResponse&& r) override {
        for (auto& i : interceptors) i(r);
        last = std::move(r);
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(Interceptor i) override { interceptors.push_back(std::move(i)); }
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

std::string query_param(const std::string& url, const std::string& name) {
    size_t pos = url.find(name + "=");
    if (pos == std::string::npos) return "";
    pos += name.size() + 1;
    size_t end = url.find('&', pos);
    return url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

std::string cookie_value(const HttpResponse& res, const std::string& name) {
    for (const auto& c : res.cookies) {
        if (c.name == name) return c.value;
    }
    return "";
}

middleware::OAuth2Config provider_config(const std::string& token_path = "/token") {
    middleware::OAuth2Config c;
    c.client_id = "client id";
    c.client_secret = "s3cr&t";
    c.redirect_uri = "https://app.example/callback?x=1";
    c.authorization_endpoint = "https://provider.example/authorize";
    c.token_endpoint = "http://127.0.0.1:" + std::to_string(kProviderPort) + token_path;
    c.userinfo_endpoint = "http://127.0.0.1:" + std::to_string(kProviderPort) + "/userinfo";
    c.scopes = {"openid", "email"};
    return c;
}

struct Flow {
    std::string location, state, verifier;
};

Flow start_login(const middleware::OAuth2& oauth) {
    auto login = oauth.login_handler();
    HttpRequest req;
    auto w = std::make_shared<CaptureWriter>();
    login(req, w);
    Flow f;
    f.location = w->last.headers["Location"];
    f.state = cookie_value(w->last, "oauth_state");
    f.verifier = cookie_value(w->last, "oauth_pkce");
    return f;
}

struct Outcome {
    bool success = false;
    std::string error;
    nlohmann::json user;
};

Outcome run_callback(const middleware::OAuth2& oauth, const std::string& query_state, const std::string& cookie_state,
                     const std::string& verifier) {
    Outcome out;
    auto cb = oauth.callback_handler(
        [&](const nlohmann::json& user, HttpRequest&, std::shared_ptr<ResponseWriter>) { out.success = true; out.user = user; },
        [&](const std::string& err, HttpRequest&, std::shared_ptr<ResponseWriter>) { out.error = err; });
    HttpRequest req;
    req.query["code"] = "4%2F0Ab_code"; // as received on the wire
    if (!query_state.empty()) req.query["state"] = query_state;
    if (!cookie_state.empty()) req.cookies["oauth_state"] = cookie_state;
    if (!verifier.empty()) req.cookies["oauth_pkce"] = verifier;
    cb(req, std::make_shared<CaptureWriter>());
    return out;
}

} // namespace

class OAuth2Test : public ::testing::Test {
protected:
    static server::App* provider;
    static std::thread provider_thread;

    static void SetUpTestSuite() {
        config::ServerConfig cfg = orbit::test::server_config();
        cfg.port = kProviderPort;
        provider = new server::App(cfg);
        provider->post("/token", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            { std::lock_guard<std::mutex> l(g_mutex); g_last_token_body = std::string(req.body); }
            HttpResponse res;
            res.set_body(R"({"access_token":"tok-123","token_type":"bearer"})", "application/json");
            w->send(std::move(res));
        });
        provider->post("/token-error", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.status(HttpStatus::BadRequest).set_body(R"({"error":"invalid_grant","client_secret":"leak"})", "application/json");
            w->send(std::move(res));
        });
        provider->post("/token-slow", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            HttpResponse res;
            res.set_body("{}");
            w->send(std::move(res));
        });
        provider->get("/userinfo", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            auto it = req.headers.find("Authorization");
            HttpResponse res;
            if (it == req.headers.end() || it->second != "Bearer tok-123") {
                res.status(HttpStatus::Unauthorized).set_body("{}");
            } else {
                res.set_body(R"({"name":"Ada"})", "application/json");
            }
            w->send(std::move(res));
        });
        provider_thread = std::thread([] { provider->listen(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    static void TearDownTestSuite() {
        provider->stop();
        // stop() only sets a flag. If no test sent the provider a request, its
        // event loop is blocked waiting for I/O with no timeout, so connect
        // once to wake it; otherwise join() never returns.
        {
            network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(kProviderPort);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            network::close_socket(fd);
        }
        if (provider_thread.joinable()) provider_thread.join();
        delete provider;
    }
};

server::App* OAuth2Test::provider = nullptr;
std::thread OAuth2Test::provider_thread;

TEST_F(OAuth2Test, LoginRedirectCarriesEncodedParamsStateAndPkce) {
    // Handlers must not depend on the OAuth2 object staying alive.
    auto login = middleware::OAuth2(provider_config()).login_handler();
    HttpRequest req;
    auto w = std::make_shared<CaptureWriter>();
    login(req, w);
    const std::string& loc = w->last.headers["Location"];
    EXPECT_EQ(w->last.status_code, HttpStatus::Found);
    EXPECT_EQ(query_param(loc, "client_id"), "client%20id");
    EXPECT_EQ(query_param(loc, "redirect_uri"), "https%3A%2F%2Fapp.example%2Fcallback%3Fx%3D1");
    EXPECT_EQ(query_param(loc, "scope"), "openid%20email");
    std::string state = cookie_value(w->last, "oauth_state");
    std::string verifier = cookie_value(w->last, "oauth_pkce");
    EXPECT_GE(state.size(), 43u);
    EXPECT_EQ(query_param(loc, "state"), state);
    EXPECT_EQ(query_param(loc, "code_challenge"), middleware::detail::pkce_challenge(verifier));
    EXPECT_EQ(query_param(loc, "code_challenge_method"), "S256");
    for (const auto& c : w->last.cookies) {
        EXPECT_TRUE(c.http_only);
        EXPECT_EQ(c.max_age, 600);
    }
}

TEST_F(OAuth2Test, CallbackWithMatchingStateCompletes) {
    middleware::OAuth2 oauth(provider_config());
    Flow f = start_login(oauth);
    Outcome out = run_callback(oauth, f.state, f.state, f.verifier);
    ASSERT_TRUE(out.success) << out.error;
    EXPECT_EQ(out.user["name"], "Ada");

    std::string body;
    { std::lock_guard<std::mutex> l(g_mutex); body = g_last_token_body; }
    EXPECT_NE(body.find("code=4%2F0Ab_code"), std::string::npos) << body;   // decoded once, encoded once
    EXPECT_NE(body.find("client_secret=s3cr%26t"), std::string::npos) << body;
    EXPECT_NE(body.find("code_verifier=" + f.verifier), std::string::npos) << body;
}

TEST_F(OAuth2Test, CallbackWithoutOrWithWrongStateIsRejected) {
    middleware::OAuth2 oauth(provider_config());
    Flow f = start_login(oauth);
    EXPECT_EQ(run_callback(oauth, "", "", f.verifier).error, "Invalid OAuth2 state");
    EXPECT_EQ(run_callback(oauth, f.state, "", f.verifier).error, "Invalid OAuth2 state");
    EXPECT_EQ(run_callback(oauth, "attacker-state", f.state, f.verifier).error, "Invalid OAuth2 state");
}

TEST_F(OAuth2Test, TokenErrorIsNotEchoed) {
    middleware::OAuth2 oauth(provider_config("/token-error"));
    Flow f = start_login(oauth);
    Outcome out = run_callback(oauth, f.state, f.state, f.verifier);
    EXPECT_FALSE(out.success);
    EXPECT_EQ(out.error.find("leak"), std::string::npos) << out.error;
}

TEST_F(OAuth2Test, SlowProviderTimesOut) {
    middleware::OAuth2Config config = provider_config("/token-slow");
    config.request_timeout_seconds = 1;
    middleware::OAuth2 oauth(config);
    Flow f = start_login(oauth);
    auto start = std::chrono::steady_clock::now();
    Outcome out = run_callback(oauth, f.state, f.state, f.verifier);
    EXPECT_FALSE(out.success);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(2500));
}

TEST(OAuth2UrlCodingTest, RoundTrips) {
    EXPECT_EQ(middleware::detail::url_encode("a b&c/d~"), "a%20b%26c%2Fd~");
    EXPECT_EQ(middleware::detail::url_decode("a%20b%26c%2Fd~+e"), "a b&c/d~ e");
    EXPECT_EQ(middleware::detail::url_decode("%zz"), "%zz");
    // RFC 7636 appendix B test vector
    EXPECT_EQ(middleware::detail::pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
              "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}
