#include <gtest/gtest.h>
#include <orbit/middleware/SessionManager.hpp>
#include <orbit/http/HttpResponse.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace http;
using middleware::MemorySessionStore;
using middleware::SessionManager;
using middleware::SessionOptions;

namespace {

// Applies interceptors when the response is "sent", like Connection does.
class SessionWriter : public ResponseWriter {
public:
    std::vector<std::function<void(HttpResponse&)>> interceptors;
    void add_interceptor(std::function<void(HttpResponse&)> i) override { interceptors.push_back(std::move(i)); }
    HttpResponse finish() {
        HttpResponse res;
        for (auto& i : interceptors) i(res);
        return res;
    }
    void send(HttpResponse&&) override {}
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

struct Outcome {
    std::string id;          // req.session_id
    std::optional<Cookie> cookie;
};

// One request: optional cookie in, `handler` runs with the session, response out.
Outcome request(SessionManager& sm, const std::string& cookie,
                const std::function<void(middleware::Session&)>& handler = nullptr) {
    HttpRequest req;
    if (!cookie.empty()) req.cookies["session_id"] = cookie;
    auto w = std::make_shared<SessionWriter>();
    EXPECT_TRUE(sm(req, w));
    EXPECT_TRUE(req.session);
    if (handler) handler(*req.session);
    HttpResponse res = w->finish();
    Outcome out{req.session_id, std::nullopt};
    if (!res.cookies.empty()) out.cookie = res.cookies[0];
    return out;
}

} // namespace

TEST(SessionStoreTest, NewVisitorGetsSessionAndCookie) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionManager sm(store);
    Outcome o = request(sm, "");
    EXPECT_EQ(o.id.size(), 64u);
    ASSERT_TRUE(o.cookie);
    EXPECT_EQ(o.cookie->value, o.id);
    EXPECT_TRUE(o.cookie->http_only);
    EXPECT_TRUE(store->load(o.id).has_value());
}

TEST(SessionStoreTest, DataPersistsAcrossRequests) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionManager sm(store);
    Outcome first = request(sm, "", [](middleware::Session& s) { s.set("user", "alice"); });

    std::string seen;
    Outcome second = request(sm, first.id, [&](middleware::Session& s) { seen = s.get("user").value_or(""); });
    EXPECT_EQ(second.id, first.id);
    EXPECT_FALSE(second.cookie) << "a known, unchanged session needs no new cookie";
    EXPECT_EQ(seen, "alice");
}

TEST(SessionStoreTest, RegenerateIssuesNewIdAndRetiresTheOld) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionManager sm(store);
    Outcome before = request(sm, "", [](middleware::Session& s) { s.set("cart", "3 items"); });

    // Login: same data, new id.
    Outcome login = request(sm, before.id, [](middleware::Session& s) {
        s.regenerate();
        s.set("user", "alice");
    });
    ASSERT_TRUE(login.cookie);
    EXPECT_NE(login.cookie->value, before.id);
    EXPECT_FALSE(store->load(before.id).has_value()) << "pre-login id must stop working";
    auto data = store->load(login.cookie->value);
    ASSERT_TRUE(data);
    EXPECT_EQ(data->at("cart"), "3 items");
    EXPECT_EQ(data->at("user"), "alice");
}

TEST(SessionStoreTest, DestroyLogsOut) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionManager sm(store);
    Outcome login = request(sm, "", [](middleware::Session& s) { s.set("user", "alice"); });
    Outcome logout = request(sm, login.id, [](middleware::Session& s) { s.destroy(); });
    ASSERT_TRUE(logout.cookie);
    EXPECT_EQ(logout.cookie->value, "");
    EXPECT_EQ(logout.cookie->max_age, 0);
    EXPECT_FALSE(store->load(login.id).has_value());

    // The old cookie now gets a brand-new, empty session.
    std::string user = "unset";
    Outcome after = request(sm, login.id, [&](middleware::Session& s) { user = s.get("user").value_or(""); });
    EXPECT_NE(after.id, login.id);
    EXPECT_EQ(user, "");
}

TEST(SessionStoreTest, ClientChosenIdIsNotAdopted) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionManager sm(store);
    std::string planted(64, 'a');
    Outcome o = request(sm, planted);
    EXPECT_NE(o.id, planted);
    EXPECT_FALSE(store->load(planted).has_value());
}

TEST(SessionStoreTest, SaveUninitializedFalseCreatesNothingUntilWritten) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionOptions opts;
    opts.save_uninitialized = false;
    SessionManager sm(store, opts);

    Outcome visitor = request(sm, "");
    EXPECT_FALSE(visitor.cookie);
    EXPECT_EQ(store->size(), 0u);

    Outcome writer = request(sm, "", [](middleware::Session& s) { s.set("lang", "en"); });
    ASSERT_TRUE(writer.cookie);
    EXPECT_EQ(store->size(), 1u);
}

TEST(SessionStoreTest, IdleSessionsExpire) {
    auto store = std::make_shared<MemorySessionStore>();
    SessionOptions opts;
    opts.ttl_seconds = 1;
    SessionManager sm(store, opts);
    Outcome first = request(sm, "", [](middleware::Session& s) { s.set("k", "v"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Outcome later = request(sm, first.id);
    EXPECT_NE(later.id, first.id);
}

TEST(SessionStoreTest, MemoryStoreIsBounded) {
    MemorySessionStore store(3);
    for (int i = 0; i < 10; ++i) {
        store.save("id" + std::to_string(i), {}, std::chrono::seconds(60 + i));
    }
    EXPECT_EQ(store.size(), 3u);
    EXPECT_TRUE(store.load("id9").has_value()); // the longest-lived survive
    EXPECT_FALSE(store.load("id0").has_value());
}

#if defined(ORBIT_ENABLE_REDIS) && !defined(_WIN32)
#include <cstdlib>

TEST(SessionStoreTest, RedisStoreKeepsDataAndRetiresOldIds) {
    constexpr int kPort = 6398;
    if (std::system("command -v redis-server >/dev/null 2>&1") != 0) GTEST_SKIP() << "redis-server not available";
    std::string start = "redis-server --port " + std::to_string(kPort) +
                        " --save '' --appendonly no --daemonize yes >/dev/null 2>&1";
    if (std::system(start.c_str()) != 0) GTEST_SKIP() << "could not start redis-server";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto store = std::make_shared<middleware::RedisSessionStore>("127.0.0.1", kPort);
    SessionManager sm(store);
    Outcome first = request(sm, "", [](middleware::Session& s) { s.set("user", "alice"); });

    std::string seen;
    request(sm, first.id, [&](middleware::Session& s) { seen = s.get("user").value_or(""); });
    EXPECT_EQ(seen, "alice");

    Outcome regen = request(sm, first.id, [](middleware::Session& s) { s.regenerate(); });
    ASSERT_TRUE(regen.cookie);
    EXPECT_FALSE(store->load(first.id).has_value());
    ASSERT_TRUE(store->load(regen.cookie->value).has_value());
    EXPECT_EQ(store->load(regen.cookie->value)->at("user"), "alice");

    request(sm, regen.cookie->value, [](middleware::Session& s) { s.destroy(); });
    EXPECT_FALSE(store->load(regen.cookie->value).has_value());

    std::string stop = "redis-cli -p " + std::to_string(kPort) + " shutdown nosave >/dev/null 2>&1";
    (void)std::system(stop.c_str());
}
#endif
