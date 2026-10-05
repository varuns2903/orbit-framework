#include <gtest/gtest.h>

#if defined(ORBIT_ENABLE_POSTGRES) && defined(__linux__)
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/network/EpollProactor.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

// PostgresClient against a throwaway server: typed rows, the
// prepared-statement cache, transactions, statement timeouts and
// reconnecting after the server drops the connection.

namespace {

constexpr int kPgPort = 5441;

using Params = std::vector<std::optional<std::string>>;

struct Results {
    bool connected = false;

    // Typed access over the wire
    std::optional<int> i;
    std::optional<double> d;
    std::optional<bool> b;
    std::optional<std::string> s;
    bool n_is_null = false;
    std::optional<int64_t> big;

    // Prepared statements
    int prepared_sum = 0;
    size_t cached_after_reuse = 0;
    std::string server_prepared_after_reuse;
    size_t cached_after_second_sql = 0;
    bool syntax_error_failed = false;
    size_t cached_after_syntax_error = 0;

    // Transactions
    std::string rows_after_rollback;
    bool commit_ok = false;
    std::string rows_after_commit;
    bool in_tx_after_error = false;
    bool commit_after_error_failed = false;
    std::string commit_after_error_message;
    bool in_tx_after_failed_commit = true;
    std::string rows_after_failed_tx;

    // Timeout
    std::string timeout_error;

    // Reconnect
    bool query_after_kill_failed = false;
    bool healthy_after_kill = true;
    bool reconnected = false;
    bool healthy_after_reconnect = false;
    size_t cached_after_reconnect = 99;
    std::string timeout_after_reconnect;

    // Cache limit
    size_t cached_at_limit = 0;
    std::string unprepared_result;
};

std::string conninfo(const std::string& socket_dir) {
    return "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres";
}

concurrency::Task scenario(std::shared_ptr<database::PostgresClient> db, std::shared_ptr<database::PostgresClient> admin,
                           Results* r, std::promise<void>* done) {
    db->set_statement_timeout(std::chrono::milliseconds(300));
    auto connect = database::connect_async(db);
    r->connected = co_await connect;
    auto connect_admin = database::connect_async(admin);
    bool admin_ok = co_await connect_admin;
    if (r->connected && admin_ok) {
        // Typed access
        auto typed_q = database::execute_async(db,
            "SELECT 42::int AS i, 3.5::float8 AS d, true AS b, 'x'::text AS s, NULL::int AS n, "
            "9223372036854775807::bigint AS big");
        database::ResultSet typed = co_await typed_q;
        if (typed.ok() && typed.size() == 1) {
            r->i = typed[0].get_as<int>("i");
            r->d = typed[0].get_as<double>("d");
            r->b = typed[0].get_as<bool>("b");
            r->s = typed[0].get_as<std::string>("s");
            r->n_is_null = typed[0].is_null("n");
            r->big = typed[0].get_as<int64_t>("big");
        }

        // Prepared statements: the same SQL three times
        for (int k = 1; k <= 3; ++k) {
            auto q = database::execute_async(db, "SELECT $1::int + 1 AS v", Params{std::to_string(k)});
            database::ResultSet res = co_await q;
            if (res.ok() && res.size() == 1) r->prepared_sum += res[0].value_or<int>("v", 0);
        }
        // typed_q above was prepared too, so two statements are cached
        r->cached_after_reuse = db->prepared_statement_count();
        auto server_q = database::query_async(db, "SELECT count(*) AS n FROM pg_prepared_statements");
        database::ResultSet server = co_await server_q;
        if (server.ok() && server.size() == 1) r->server_prepared_after_reuse = server[0].get("n").value_or("");
        auto second = database::execute_async(db, "SELECT $1::text AS v", Params{std::string("y")});
        co_await second;
        r->cached_after_second_sql = db->prepared_statement_count();
        auto bad = database::execute_async(db, "SELEC 1");
        database::ResultSet bad_res = co_await bad;
        r->syntax_error_failed = !bad_res.ok();
        r->cached_after_syntax_error = db->prepared_statement_count();

        // Transactions
        auto create = database::query_async(db, "CREATE TABLE tx_items (v int)");
        co_await create;
        auto begin1 = database::begin_async(db);
        co_await begin1;
        auto ins1 = database::execute_async(db, "INSERT INTO tx_items VALUES ($1)", Params{std::string("1")});
        co_await ins1;
        auto rb = database::rollback_async(db);
        co_await rb;
        auto count1 = database::query_async(db, "SELECT count(*) AS n FROM tx_items");
        database::ResultSet c1 = co_await count1;
        if (c1.ok() && c1.size() == 1) r->rows_after_rollback = c1[0].get("n").value_or("");

        auto begin2 = database::begin_async(db);
        co_await begin2;
        auto ins2 = database::execute_async(db, "INSERT INTO tx_items VALUES ($1)", Params{std::string("2")});
        co_await ins2;
        auto commit2 = database::commit_async(db);
        database::ResultSet committed = co_await commit2;
        r->commit_ok = committed.ok();
        auto count2 = database::query_async(db, "SELECT count(*) AS n FROM tx_items");
        database::ResultSet c2 = co_await count2;
        if (c2.ok() && c2.size() == 1) r->rows_after_commit = c2[0].get("n").value_or("");

        auto begin3 = database::begin_async(db);
        co_await begin3;
        auto ins3 = database::execute_async(db, "INSERT INTO tx_items VALUES ($1)", Params{std::string("3")});
        co_await ins3;
        auto div0 = database::query_async(db, "SELECT 1/0");
        co_await div0;
        r->in_tx_after_error = db->in_transaction();
        auto commit3 = database::commit_async(db);
        database::ResultSet failed_commit = co_await commit3;
        r->commit_after_error_failed = !failed_commit.ok();
        r->commit_after_error_message = failed_commit.error();
        r->in_tx_after_failed_commit = db->in_transaction();
        auto count3 = database::query_async(db, "SELECT count(*) AS n FROM tx_items");
        database::ResultSet c3 = co_await count3;
        if (c3.ok() && c3.size() == 1) r->rows_after_failed_tx = c3[0].get("n").value_or("");

        // Statement timeout
        auto sleep = database::query_async(db, "SELECT pg_sleep(3)");
        database::ResultSet slept = co_await sleep;
        r->timeout_error = slept.error();

        // The server drops the connection
        auto pid_q = database::query_async(db, "SELECT pg_backend_pid() AS pid");
        database::ResultSet pid = co_await pid_q;
        std::string backend = pid.ok() && pid.size() == 1 ? pid[0].get("pid").value_or("") : "";
        auto kill = database::query_async(admin, "SELECT pg_terminate_backend($1::int)", Params{backend});
        co_await kill;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto after_kill = database::query_async(db, "SELECT 1");
        database::ResultSet dead = co_await after_kill;
        r->query_after_kill_failed = !dead.ok();
        r->healthy_after_kill = db->is_healthy();
        auto reconnect = database::connect_async(db);
        r->reconnected = co_await reconnect;
        r->healthy_after_reconnect = db->is_healthy();
        r->cached_after_reconnect = db->prepared_statement_count();
        auto show = database::query_async(db, "SHOW statement_timeout");
        database::ResultSet shown = co_await show;
        if (shown.ok() && shown.size() == 1) r->timeout_after_reconnect = shown[0].get(0).value_or("");

        // Cache limit: beyond it statements still run, unprepared
        db->set_max_prepared_statements(1);
        auto p1 = database::execute_async(db, "SELECT 1 AS v");
        co_await p1;
        auto p2 = database::execute_async(db, "SELECT $1::text AS v", Params{std::string("unprepared")});
        database::ResultSet unprepared = co_await p2;
        r->cached_at_limit = db->prepared_statement_count();
        if (unprepared.ok() && unprepared.size() == 1) r->unprepared_result = unprepared[0].get("v").value_or("");
    }
    done->set_value();
}

} // namespace

class PostgresClientTest : public ::testing::Test {
protected:
    static std::string data_dir, socket_dir;
    static bool available;

    static void SetUpTestSuite() {
        if (std::system("command -v initdb >/dev/null 2>&1 && command -v pg_ctl >/dev/null 2>&1") != 0) return;
        auto base = std::filesystem::temp_directory_path() / ("orbit_pgclient_test_" + std::to_string(::getpid()));
        std::filesystem::remove_all(base);
        data_dir = (base / "data").string();
        socket_dir = (base / "sock").string();
        std::filesystem::create_directories(socket_dir);
        std::string init = "initdb -D '" + data_dir + "' -U postgres -A trust >/dev/null 2>&1";
        std::string start = "pg_ctl -D '" + data_dir + "' -w -l '" + (base / "log").string() +
                            "' -o \"-p " + std::to_string(kPgPort) + " -k " + socket_dir +
                            " -c listen_addresses=''\" start >/dev/null 2>&1";
        available = std::system(init.c_str()) == 0 && std::system(start.c_str()) == 0;
    }

    static void TearDownTestSuite() {
        if (!data_dir.empty()) {
            std::string stop = "pg_ctl -D '" + data_dir + "' -m immediate stop >/dev/null 2>&1";
            (void)std::system(stop.c_str());
            std::filesystem::remove_all(std::filesystem::path(data_dir).parent_path());
        }
    }

    void SetUp() override {
        if (available) return;
        // CI sets this, so a runner without initdb/pg_ctl fails instead of
        // skipping the PostgreSQL tests unnoticed.
        const char* required = std::getenv("ORBIT_REQUIRE_POSTGRES_TESTS");
        if (required && *required) FAIL() << "could not start a PostgreSQL server";
        GTEST_SKIP() << "PostgreSQL server binaries not available";
    }
};

std::string PostgresClientTest::data_dir;
std::string PostgresClientTest::socket_dir;
bool PostgresClientTest::available = false;

TEST_F(PostgresClientTest, PreparedTransactionsTimeoutsAndReconnect) {
    network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });

    auto db = std::make_shared<database::PostgresClient>(&proactor, conninfo(socket_dir));
    auto admin = std::make_shared<database::PostgresClient>(&proactor, conninfo(socket_dir));
    Results r;
    std::promise<void> done;
    auto finished = done.get_future();
    scenario(db, admin, &r, &done);
    bool completed = finished.wait_for(std::chrono::seconds(30)) == std::future_status::ready;

    running = false;
    loop.join();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(r.connected);

    EXPECT_EQ(r.i, 42);
    EXPECT_EQ(r.d, 3.5);
    EXPECT_EQ(r.b, true);
    EXPECT_EQ(r.s, "x");
    EXPECT_TRUE(r.n_is_null);
    EXPECT_EQ(r.big, 9223372036854775807LL);

    EXPECT_EQ(r.prepared_sum, 2 + 3 + 4);
    EXPECT_EQ(r.cached_after_reuse, 2u);
    EXPECT_EQ(r.server_prepared_after_reuse, "2") << "the plan was reused, not prepared again";
    EXPECT_EQ(r.cached_after_second_sql, 3u);
    EXPECT_TRUE(r.syntax_error_failed);
    EXPECT_EQ(r.cached_after_syntax_error, 3u);

    EXPECT_EQ(r.rows_after_rollback, "0");
    EXPECT_TRUE(r.commit_ok);
    EXPECT_EQ(r.rows_after_commit, "1");
    EXPECT_TRUE(r.in_tx_after_error);
    EXPECT_TRUE(r.commit_after_error_failed);
    EXPECT_NE(r.commit_after_error_message.find("rolled back"), std::string::npos) << r.commit_after_error_message;
    EXPECT_FALSE(r.in_tx_after_failed_commit);
    EXPECT_EQ(r.rows_after_failed_tx, "1");

    EXPECT_NE(r.timeout_error.find("statement timeout"), std::string::npos) << r.timeout_error;

    EXPECT_TRUE(r.query_after_kill_failed);
    EXPECT_FALSE(r.healthy_after_kill);
    EXPECT_TRUE(r.reconnected);
    EXPECT_TRUE(r.healthy_after_reconnect);
    EXPECT_EQ(r.cached_after_reconnect, 0u) << "prepared statements belong to the old session";
    EXPECT_EQ(r.timeout_after_reconnect, "300ms") << "the timeout is applied to the new session";

    EXPECT_EQ(r.cached_at_limit, 1u);
    EXPECT_EQ(r.unprepared_result, "unprepared");
}

#endif
