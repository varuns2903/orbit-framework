#include <gtest/gtest.h>
#include <orbit/utils/Random.hpp>
#include <orbit/middleware/Csrf.hpp>
#ifdef ORBIT_ENABLE_REDIS
#include <orbit/middleware/SessionManager.hpp>
#endif
#include <orbit/http/HttpResponse.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace http;

namespace {

bool is_lower_hex(const std::string& s) {
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

class InterceptingWriter : public ResponseWriter {
public:
    std::vector<std::function<void(HttpResponse&)>> interceptors;

    // Runs the registered interceptors on an empty response, as Connection::send does.
    HttpResponse finish() {
        HttpResponse res;
        for (auto& i : interceptors) i(res);
        return res;
    }

    void send(HttpResponse&&) override {}
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override { interceptors.push_back(std::move(interceptor)); }
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

} // namespace

TEST(SecureRandomTest, ProducesHexOfRequestedLength) {
    std::string token = utils::secure_random_hex(32);
    EXPECT_EQ(token.size(), 64u);
    EXPECT_TRUE(is_lower_hex(token));
}

TEST(SecureRandomTest, DoesNotRepeat) {
    std::set<std::string> seen;
    for (int i = 0; i < 2000; ++i) seen.insert(utils::secure_random_hex(16));
    EXPECT_EQ(seen.size(), 2000u);
}

TEST(SecureRandomTest, ConstantTimeEquals) {
    EXPECT_TRUE(utils::constant_time_equals("abc", "abc"));
    EXPECT_FALSE(utils::constant_time_equals("abc", "abd"));
    EXPECT_FALSE(utils::constant_time_equals("abc", "abcd"));
    EXPECT_TRUE(utils::constant_time_equals("", ""));
}

TEST(SecureRandomTest, CsrfTokensAre256Bit) {
    std::string a = middleware::Csrf::generate_random_token();
    std::string b = middleware::Csrf::generate_random_token();
    EXPECT_EQ(a.size(), 64u);
    EXPECT_TRUE(is_lower_hex(a));
    EXPECT_NE(a, b);
}

#ifdef ORBIT_ENABLE_REDIS

// --- SessionManager against a real Redis (skipped when redis-server is absent) ---

class SessionManagerRedisTest : public ::testing::Test {
protected:
    static constexpr int kRedisPort = 6397;
    static bool redis_available;

    static void SetUpTestSuite() {
#ifndef _WIN32
        if (std::system("command -v redis-server >/dev/null 2>&1") != 0) return;
        std::string cmd = "redis-server --port " + std::to_string(kRedisPort) +
                          " --save '' --appendonly no --daemonize yes >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        redis_available = true;
#endif
    }

    static void TearDownTestSuite() {
        if (redis_available) {
            std::string cmd = "redis-cli -p " + std::to_string(kRedisPort) + " shutdown nosave >/dev/null 2>&1";
            (void)std::system(cmd.c_str());
        }
    }

    void SetUp() override {
        if (!redis_available) GTEST_SKIP() << "redis-server not available";
    }

    // Runs the middleware and returns (req.session_id, Set-Cookie value or "").
    static std::pair<std::string, std::string> run(middleware::SessionManager& sm, const std::string& cookie) {
        HttpRequest req;
        if (!cookie.empty()) req.cookies["session_id"] = cookie;
        auto w = std::make_shared<InterceptingWriter>();
        EXPECT_TRUE(sm(req, w));
        HttpResponse res = w->finish();
        return {req.session_id, res.cookies.empty() ? "" : res.cookies[0].value};
    }
};

bool SessionManagerRedisTest::redis_available = false;

TEST_F(SessionManagerRedisTest, IssuesRandomSessionAndCookie) {
    middleware::SessionManager sm("127.0.0.1", kRedisPort);
    auto [id, cookie] = run(sm, "");
    EXPECT_EQ(id.size(), 64u);
    EXPECT_TRUE(is_lower_hex(id));
    EXPECT_EQ(cookie, id);
}

TEST_F(SessionManagerRedisTest, KeepsKnownSession) {
    middleware::SessionManager sm("127.0.0.1", kRedisPort);
    auto [id, cookie] = run(sm, "");
    auto [id2, cookie2] = run(sm, id);
    EXPECT_EQ(id2, id);
    EXPECT_TRUE(cookie2.empty()); // no new cookie needed
}

TEST_F(SessionManagerRedisTest, RejectsClientChosenSessionId) {
    middleware::SessionManager sm("127.0.0.1", kRedisPort);
    std::string planted(64, 'a'); // well-formed, but never issued
    auto [id, cookie] = run(sm, planted);
    EXPECT_NE(id, planted);
    EXPECT_EQ(cookie, id);
}

TEST_F(SessionManagerRedisTest, RejectsMalformedSessionId) {
    middleware::SessionManager sm("127.0.0.1", kRedisPort);
    auto [id, cookie] = run(sm, "attacker-chosen");
    EXPECT_NE(id, "attacker-chosen");
    EXPECT_EQ(id.size(), 64u);
}

TEST_F(SessionManagerRedisTest, CookieAttributesFollowOptions) {
    middleware::SessionOptions opts;
    opts.secure = true;
    opts.same_site = "Strict";
    opts.ttl_seconds = 600;
    middleware::SessionManager sm("127.0.0.1", kRedisPort, opts);
    HttpRequest req;
    auto w = std::make_shared<InterceptingWriter>();
    sm(req, w);
    HttpResponse res = w->finish();
    ASSERT_EQ(res.cookies.size(), 1u);
    EXPECT_TRUE(res.cookies[0].secure);
    EXPECT_TRUE(res.cookies[0].http_only);
    EXPECT_EQ(res.cookies[0].same_site, "Strict");
    EXPECT_EQ(res.cookies[0].max_age, 600);
}

#endif // ORBIT_ENABLE_REDIS
