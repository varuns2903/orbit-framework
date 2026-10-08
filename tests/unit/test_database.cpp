#include <gtest/gtest.h>
#include <orbit/database/ConnectionPool.hpp>
#include <orbit/database/ResultSet.hpp>

#include <cmath>
#include <cstdint>

using namespace orbit::database;

TEST(ConnectionPoolTest, AcquireRelease) {
    auto factory = []() { return std::make_shared<int>(42); };
    auto pool = std::make_shared<ConnectionPool<int>>(2, factory);
    
    // Init synchronously for the test
    bool init_success = false;
    pool->init([](std::shared_ptr<int> client, std::function<void(bool)> cb) {
        cb(true); // Always succeeds
    }, [&](bool success) {
        init_success = success;
    });
    EXPECT_TRUE(init_success);
    
    bool acquired = false;
    pool->acquire([&](std::shared_ptr<int> client) {
        EXPECT_EQ(*client, 42);
        acquired = true;
        pool->release(client);
    });
    
    EXPECT_TRUE(acquired);
}

namespace {

struct FakeClient {
    int id = 0;
    bool healthy = true;
};

// Collects scheduled tasks so a test decides when "time passes".
struct ManualScheduler {
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> tasks;
    void run_next() {
        auto task = std::move(tasks.front().second);
        tasks.erase(tasks.begin());
        task();
    }
};

struct HealthPoolFixture {
    std::shared_ptr<ManualScheduler> scheduler = std::make_shared<ManualScheduler>();
    int next_id = 0;
    int connect_failures_left = 0; // the next N connects fail
    std::shared_ptr<ConnectionPool<FakeClient>> pool;

    explicit HealthPoolFixture(size_t size) {
        PoolOptions<FakeClient> opts;
        opts.health_check = [](FakeClient& c) { return c.healthy; };
        opts.initial_backoff = std::chrono::milliseconds(100);
        opts.max_backoff = std::chrono::milliseconds(350);
        auto sched = scheduler;
        opts.schedule = [sched](std::chrono::milliseconds d, std::function<void()> t) {
            sched->tasks.emplace_back(d, std::move(t));
        };
        pool = std::make_shared<ConnectionPool<FakeClient>>(size, [this] {
            auto c = std::make_shared<FakeClient>();
            c->id = ++next_id;
            return c;
        }, opts);
    }

    void init(bool* ok = nullptr) {
        pool->init([this](std::shared_ptr<FakeClient>, std::function<void(bool)> cb) {
            if (connect_failures_left > 0) {
                --connect_failures_left;
                cb(false);
            } else {
                cb(true);
            }
        }, [ok](bool success) { if (ok) *ok = success; });
    }

    std::shared_ptr<FakeClient> acquire_now() {
        std::shared_ptr<FakeClient> got;
        pool->acquire([&got](std::shared_ptr<FakeClient> c) { got = std::move(c); });
        return got;
    }
};

} // namespace

TEST(ConnectionPoolHealthTest, UnhealthyIdleClientIsReplacedOnAcquire) {
    HealthPoolFixture f(2);
    f.init();
    auto first = f.acquire_now();
    ASSERT_TRUE(first);
    f.pool->release(first);
    ASSERT_EQ(f.pool->idle_count(), 2u);

    // Both idle clients die (e.g. the server restarted).
    auto a = f.acquire_now();
    auto b = f.acquire_now();
    a->healthy = false;
    b->healthy = false;
    f.pool->release(a); // dropped and replaced at once
    f.pool->release(b);
    EXPECT_EQ(f.pool->idle_count(), 2u);
    auto c = f.acquire_now();
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->healthy);
    EXPECT_GT(c->id, 2) << "a replacement, not one of the dead clients";
}

TEST(ConnectionPoolHealthTest, DeadIdleClientIsSkippedNotHandedOut) {
    HealthPoolFixture f(2);
    f.init();
    auto a = f.acquire_now();
    auto b = f.acquire_now();
    f.pool->release(a);
    f.pool->release(b);
    a->healthy = false; // dies while idle
    auto got = f.acquire_now();
    ASSERT_TRUE(got);
    EXPECT_NE(got.get(), a.get());
    EXPECT_TRUE(got->healthy);
    EXPECT_EQ(f.pool->idle_count(), 1u) << "the replacement is ready";
}

TEST(ConnectionPoolHealthTest, WaitersGetTheReplacement) {
    HealthPoolFixture f(1);
    f.init();
    auto only = f.acquire_now();
    std::shared_ptr<FakeClient> waiter_got;
    f.pool->acquire([&](std::shared_ptr<FakeClient> c) { waiter_got = std::move(c); });
    EXPECT_EQ(f.pool->waiting_count(), 1u);
    only->healthy = false;
    f.pool->release(only);
    ASSERT_TRUE(waiter_got);
    EXPECT_TRUE(waiter_got->healthy);
    EXPECT_EQ(f.pool->waiting_count(), 0u);
}

TEST(ConnectionPoolHealthTest, FailedReconnectsBackOffExponentiallyAndCap) {
    HealthPoolFixture f(1);
    f.init();
    auto only = f.acquire_now();
    std::shared_ptr<FakeClient> waiter_got;
    f.pool->acquire([&](std::shared_ptr<FakeClient> c) { waiter_got = std::move(c); });

    f.connect_failures_left = 4; // the database is down for a while
    only->healthy = false;
    f.pool->release(only); // immediate attempt fails
    std::vector<long long> delays;
    while (!f.scheduler->tasks.empty()) {
        delays.push_back(f.scheduler->tasks.front().first.count());
        EXPECT_EQ(f.pool->reconnecting_count(), 1u);
        f.scheduler->run_next();
    }
    EXPECT_EQ(delays, (std::vector<long long>{100, 200, 350, 350}));
    ASSERT_TRUE(waiter_got) << "served once the database came back";
    EXPECT_EQ(f.pool->reconnecting_count(), 0u);
}

TEST(ConnectionPoolHealthTest, StartupFailuresAreRetried) {
    HealthPoolFixture f(2);
    f.connect_failures_left = 1;
    bool ok = true;
    f.init(&ok);
    EXPECT_FALSE(ok) << "init still reports the failure";
    EXPECT_EQ(f.pool->idle_count(), 1u);
    ASSERT_EQ(f.scheduler->tasks.size(), 1u);
    f.scheduler->run_next();
    EXPECT_EQ(f.pool->idle_count(), 2u);
}

TEST(ConnectionPoolHealthTest, PendingRetryOutlivingThePoolIsHarmless) {
    std::shared_ptr<ManualScheduler> scheduler;
    {
        HealthPoolFixture f(1);
        f.init();
        scheduler = f.scheduler;
        f.connect_failures_left = 100;
        auto only = f.acquire_now();
        only->healthy = false;
        f.pool->release(only);
        ASSERT_EQ(scheduler->tasks.size(), 1u);
    }
    scheduler->run_next(); // the pool is gone: nothing happens
    EXPECT_TRUE(scheduler->tasks.empty());
}

TEST(ConnectionPoolHealthTest, WithoutHealthCheckNothingIsReplaced) {
    auto pool = std::make_shared<ConnectionPool<FakeClient>>(1, [] { return std::make_shared<FakeClient>(); });
    pool->init([](std::shared_ptr<FakeClient>, std::function<void(bool)> cb) { cb(true); }, [](bool) {});
    std::shared_ptr<FakeClient> got;
    pool->acquire([&](std::shared_ptr<FakeClient> c) { got = std::move(c); });
    got->healthy = false;
    pool->release(got);
    std::shared_ptr<FakeClient> again;
    pool->acquire([&](std::shared_ptr<FakeClient> c) { again = std::move(c); });
    EXPECT_EQ(again.get(), got.get());
}

namespace {

orbit::database::Row typed_row() {
    auto cols = std::make_shared<std::unordered_map<std::string, size_t>>();
    std::vector<std::optional<std::string>> vals;
    auto add = [&](const std::string& name, std::optional<std::string> v) {
        (*cols)[name] = vals.size();
        vals.push_back(std::move(v));
    };
    add("int", "42");
    add("neg", "-7");
    add("big", "9223372036854775807");
    add("float", "3.25");
    add("sci", "1e-3");
    add("t", "t");
    add("f", "false");
    add("one", "1");
    add("text", "hello");
    add("null", std::nullopt);
    add("junk", "12abc");
    add("empty", "");
    add("nan", "NaN");
    return orbit::database::Row(std::move(vals), cols);
}

} // namespace

TEST(RowTypedAccessTest, ConvertsExactly) {
    auto row = typed_row();
    EXPECT_EQ(row.get_as<int>("int"), 42);
    EXPECT_EQ(row.get_as<int>("neg"), -7);
    EXPECT_EQ(row.get_as<int64_t>("big"), 9223372036854775807LL);
    EXPECT_EQ(row.get_as<int>("big"), std::nullopt) << "out of range for int";
    EXPECT_EQ(row.get_as<unsigned>("neg"), std::nullopt);
    EXPECT_DOUBLE_EQ(*row.get_as<double>("float"), 3.25);
    EXPECT_DOUBLE_EQ(*row.get_as<double>("sci"), 0.001);
    EXPECT_TRUE(std::isnan(*row.get_as<double>("nan")));
    EXPECT_EQ(row.get_as<bool>("t"), true);
    EXPECT_EQ(row.get_as<bool>("f"), false);
    EXPECT_EQ(row.get_as<bool>("one"), true);
    EXPECT_EQ(row.get_as<std::string>("text"), "hello");
    EXPECT_EQ(row.get_as<int>(size_t{0}), 42);
}

TEST(RowTypedAccessTest, NullMissingAndJunkAreNullopt) {
    auto row = typed_row();
    EXPECT_EQ(row.get_as<int>("null"), std::nullopt);
    EXPECT_EQ(row.get_as<std::string>("null"), std::nullopt);
    EXPECT_EQ(row.get_as<int>("no_such_column"), std::nullopt);
    EXPECT_EQ(row.get_as<int>("junk"), std::nullopt) << "trailing characters";
    EXPECT_EQ(row.get_as<double>("junk"), std::nullopt);
    EXPECT_EQ(row.get_as<int>("empty"), std::nullopt);
    EXPECT_EQ(row.get_as<bool>("text"), std::nullopt);
    EXPECT_EQ(row.get_as<int>(size_t{99}), std::nullopt);
}

TEST(RowTypedAccessTest, ValueOrAndIsNull) {
    auto row = typed_row();
    EXPECT_EQ(row.value_or<int>("int", -1), 42);
    EXPECT_EQ(row.value_or<int>("null", -1), -1);
    EXPECT_EQ(row.value_or<std::string>("missing", "dflt"), "dflt");
    EXPECT_TRUE(row.is_null("null"));
    EXPECT_FALSE(row.is_null("int"));
    EXPECT_FALSE(row.is_null("missing"));
}
