#include <gtest/gtest.h>

#ifdef ORBIT_ENABLE_REDIS
#include <orbit/database/RedisClient.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <orbit/middleware/DistributedRateLimiter.hpp>
#include <stdexcept>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr int kPort = 6396;

bool start_redis(int port = kPort) {
#ifdef _WIN32
    return false;
#else
    if (std::system("command -v redis-server >/dev/null 2>&1") != 0) return false;
    std::string cmd = "redis-server --port " + std::to_string(port) +
                      " --save '' --appendonly no --daemonize yes >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return true;
#endif
}

// CI sets ORBIT_REQUIRE_REDIS_TESTS, so a runner without redis-server fails
// instead of skipping these tests unnoticed.
bool redis_required() {
    const char* required = std::getenv("ORBIT_REQUIRE_REDIS_TESTS");
    return required && *required;
}

#define ORBIT_START_REDIS_OR_SKIP(...)                                                  \
    do {                                                                                \
        if (!start_redis(__VA_ARGS__)) {                                                \
            if (redis_required()) FAIL() << "could not start redis-server";             \
            GTEST_SKIP() << "redis-server not available";                               \
        }                                                                               \
    } while (0)

void stop_redis(int port = kPort) {
    std::string cmd = "redis-cli -p " + std::to_string(port) + " shutdown nosave >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// Runs fn on a detached thread; returns false if it has not finished in time.
// A deadlocked call would otherwise hang the whole test binary.
template <typename Fn>
bool finishes_within(std::chrono::milliseconds limit, Fn fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done, fn]() mutable {
        fn();
        *done = true;
    }).detach();
    auto deadline = std::chrono::steady_clock::now() + limit;
    while (!*done && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return *done;
}

// Records the last response; everything else is unused by the rate limiter.
class CaptureWriter : public http::ResponseWriter {
public:
    http::HttpResponse last;
    void send(http::HttpResponse&& response) override { last = std::move(response); }
    void send_headers(http::HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(http::HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

} // namespace

TEST(RedisClientTest, ReconnectsAfterServerRestart) {
    ORBIT_START_REDIS_OR_SKIP();

    // Leaked on purpose: if the fix regresses, a deadlocked thread may still hold it.
    auto* client = new database::RedisClient("127.0.0.1", kPort);
    EXPECT_EQ(client->incr("orbit:test:counter"), 1);

    stop_redis();

    // The first call after the restart discovers the dead socket.
    bool first_done = finishes_within(std::chrono::seconds(5), [client] { client->incr("orbit:test:counter"); });
    ASSERT_TRUE(first_done) << "RedisClient deadlocked after losing its connection";

    ASSERT_TRUE(start_redis());
    long long value = 0;
    bool second_done = finishes_within(std::chrono::seconds(5), [client, &value] { value = client->incr("orbit:test:counter"); });
    ASSERT_TRUE(second_done) << "RedisClient deadlocked while reconnecting";
    EXPECT_EQ(value, 1); // fresh server, fresh counter

    auto got = client->get("orbit:test:missing");
    EXPECT_FALSE(got.has_value());
    EXPECT_TRUE(client->set("orbit:test:key", "v", 10));
    EXPECT_EQ(client->get("orbit:test:key").value_or(""), "v");

    stop_redis();
    delete client;
}

TEST(RedisClientTest, FailsFastWhenNothingListens) {
    database::RedisClient client("127.0.0.1", 1); // port 1: connection refused
    bool done = finishes_within(std::chrono::seconds(5), [&client] { EXPECT_EQ(client.incr("k"), 0); });
    EXPECT_TRUE(done);
}

// Separate port so this can run in parallel with the restart test above.
constexpr int kLimiterPort = 6397;

TEST(RedisClientTest, IncrWithExpirySetsTtl) {
    ORBIT_START_REDIS_OR_SKIP(kLimiterPort);
    database::RedisClient client("127.0.0.1", kLimiterPort);

    EXPECT_EQ(client.incr_with_expiry("orbit:test:window", 1), 1);
    EXPECT_EQ(client.incr_with_expiry("orbit:test:window", 1), 2);
    // A plain INCR never expires; this key must, or the client stays locked out.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(client.incr_with_expiry("orbit:test:window", 1), 1);

    stop_redis(kLimiterPort);
}

TEST(DistributedRateLimiterTest, RejectsOverLimitWithRetryAfter) {
    ORBIT_START_REDIS_OR_SKIP(kLimiterPort);
    auto m = middleware::distributed_rate_limit("127.0.0.1", kLimiterPort, 2, std::chrono::seconds(30));
    http::HttpRequest req;
    req.client_ip = "192.0.2.10";
    auto writer = std::make_shared<CaptureWriter>();

    EXPECT_TRUE(m(req, writer));
    EXPECT_TRUE(m(req, writer));
    EXPECT_FALSE(m(req, writer));
    EXPECT_EQ(writer->last.status_code, http::HttpStatus::TooManyRequests);
    EXPECT_EQ(writer->last.headers["Retry-After"], "30");
    EXPECT_EQ(writer->last.headers.count("Connection"), 0u);

    stop_redis(kLimiterPort);
}

TEST(DistributedRateLimiterTest, RedisDownFailsClosedByDefault) {
    http::HttpRequest req;
    req.client_ip = "192.0.2.11";
    auto writer = std::make_shared<CaptureWriter>();

    auto closed = middleware::distributed_rate_limit("127.0.0.1", 1, 10, std::chrono::seconds(30));
    EXPECT_FALSE(closed(req, writer));
    EXPECT_EQ(writer->last.status_code, http::HttpStatus::ServiceUnavailable);

    auto open = middleware::distributed_rate_limit("127.0.0.1", 1, 10, std::chrono::seconds(30), nullptr, true);
    EXPECT_TRUE(open(req, writer));
}

// Separate port again, so these can run in parallel with the tests above.
constexpr int kCommandsPort = 6398;

TEST(RedisClientTest, PingSetGetDelAndExpire) {
    ORBIT_START_REDIS_OR_SKIP(kCommandsPort);
    database::RedisClient client("127.0.0.1", kCommandsPort);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(client.ping(), "PONG");

    EXPECT_TRUE(client.set("orbit:test:k", "v"));
    EXPECT_EQ(client.get("orbit:test:k"), "v");
    EXPECT_TRUE(client.del("orbit:test:k"));
    EXPECT_FALSE(client.del("orbit:test:k")) << "nothing left to delete";
    EXPECT_EQ(client.get("orbit:test:k"), std::nullopt);

    EXPECT_FALSE(client.expire("orbit:test:absent", 10));
    EXPECT_TRUE(client.set("orbit:test:short", "v"));
    EXPECT_TRUE(client.expire("orbit:test:short", 1));
    EXPECT_TRUE(client.set("orbit:test:ex", "v", 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_EQ(client.get("orbit:test:short"), std::nullopt);
    EXPECT_EQ(client.get("orbit:test:ex"), std::nullopt);

    stop_redis(kCommandsPort);
}

// GET of a key holding "" is a value, not a missing key.
TEST(RedisClientTest, EmptyValueIsNotMissing) {
    ORBIT_START_REDIS_OR_SKIP(kCommandsPort);
    database::RedisClient client("127.0.0.1", kCommandsPort);
    ASSERT_TRUE(client.set("orbit:test:empty", ""));
    auto got = client.get("orbit:test:empty");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "");
    EXPECT_EQ(client.get("orbit:test:never-set"), std::nullopt);
    stop_redis(kCommandsPort);
}

TEST(RedisClientTest, ValuesAreBinarySafe) {
    ORBIT_START_REDIS_OR_SKIP(kCommandsPort);
    database::RedisClient client("127.0.0.1", kCommandsPort);
    using namespace std::string_literals;
    const std::string framing = "line\r\n$5\r\n*1\r\nNUL\0end"s;
    ASSERT_TRUE(client.set("orbit:test:framing", framing));
    EXPECT_EQ(client.get("orbit:test:framing"), framing);

    // Bigger than one socket read, so the bulk reply arrives in pieces.
    std::string big(3 * 1024 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    ASSERT_TRUE(client.set("orbit:test:big", big));
    EXPECT_EQ(client.get("orbit:test:big"), big);
    stop_redis(kCommandsPort);
}

// An error reply (-ERR ...) is not a value, and leaves the connection in
// step for the next command.
TEST(RedisClientTest, ErrorRepliesKeepTheConnectionInSync) {
    ORBIT_START_REDIS_OR_SKIP(kCommandsPort);
    database::RedisClient client("127.0.0.1", kCommandsPort);
    ASSERT_TRUE(client.set("orbit:test:text", "not a number"));
    EXPECT_EQ(client.incr("orbit:test:text"), 0);
    EXPECT_EQ(client.get("orbit:test:text"), "not a number");
    EXPECT_EQ(client.incr("orbit:test:n"), 1);
    stop_redis(kCommandsPort);
}

TEST(RedisClientTest, DisconnectThenReconnectsOnTheNextCommand) {
    ORBIT_START_REDIS_OR_SKIP(kCommandsPort);
    database::RedisClient client("127.0.0.1", kCommandsPort);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(client.incr("orbit:test:c"), 1);
    client.disconnect();
    client.disconnect();
    EXPECT_EQ(client.incr("orbit:test:c"), 2);
    stop_redis(kCommandsPort);
}

TEST(RedisClientTest, CommandsFailCleanlyWithoutAServer) {
    database::RedisClient client("127.0.0.1", 1);
    EXPECT_FALSE(client.connect());
    EXPECT_EQ(client.ping(), "");
    EXPECT_FALSE(client.set("k", "v"));
    EXPECT_EQ(client.get("k"), std::nullopt);
    EXPECT_FALSE(client.del("k"));
    EXPECT_FALSE(client.expire("k", 1));
    EXPECT_EQ(client.incr_with_expiry("k", 1), 0);
}

TEST(RedisClientTest, UnresolvableHostFailsToConnect) {
    database::RedisClient client("orbit-no-such-host.invalid", 6379);
    EXPECT_FALSE(client.connect());
    EXPECT_EQ(client.get("k"), std::nullopt);
}

#endif // ORBIT_ENABLE_REDIS
