#include <gtest/gtest.h>

#if defined(ORBIT_ENABLE_MARIADB) && defined(__linux__)
#include <orbit/database/MysqlClient.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/network/EpollProactor.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

// MysqlClient against a real MySQL or MariaDB server. The server comes from
// the environment, as CI starts one in a service container:
//
//   ORBIT_TEST_MYSQL_PORT      required to run these tests
//   ORBIT_TEST_MYSQL_HOST      default 127.0.0.1
//   ORBIT_TEST_MYSQL_USER      default root
//   ORBIT_TEST_MYSQL_PASSWORD  default empty
//   ORBIT_TEST_MYSQL_DATABASE  default orbit_test (must exist)
//
// Without ORBIT_TEST_MYSQL_PORT the tests skip, unless
// ORBIT_REQUIRE_MYSQL_TESTS is set, in which case they fail.

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

database::MysqlClient::Config server_config() {
    database::MysqlClient::Config c;
    c.host = env_or("ORBIT_TEST_MYSQL_HOST", "127.0.0.1");
    c.port = std::stoi(env_or("ORBIT_TEST_MYSQL_PORT", "3306"));
    c.user = env_or("ORBIT_TEST_MYSQL_USER", "root");
    c.password = env_or("ORBIT_TEST_MYSQL_PASSWORD", "");
    c.dbname = env_or("ORBIT_TEST_MYSQL_DATABASE", "orbit_test");
    return c;
}

// Runs the event loop on a thread while a coroutine scenario uses it.
class Loop {
public:
    Loop() : thread_([this] { while (running_) proactor.run_once(20); }) {}
    ~Loop() {
        running_ = false;
        thread_.join();
    }
    network::EpollProactor proactor;

private:
    std::atomic<bool> running_{true};
    std::thread thread_;
};

// Starts a scenario coroutine and waits for it to signal completion.
template <typename F>
bool run_scenario(F&& start, std::chrono::seconds limit = std::chrono::seconds(30)) {
    std::promise<void> done;
    auto finished = done.get_future();
    start(&done);
    return finished.wait_for(limit) == std::future_status::ready;
}

} // namespace

class MysqlClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (std::getenv("ORBIT_TEST_MYSQL_PORT")) return;
        const char* required = std::getenv("ORBIT_REQUIRE_MYSQL_TESTS");
        if (required && *required) FAIL() << "ORBIT_REQUIRE_MYSQL_TESTS is set but ORBIT_TEST_MYSQL_PORT is not";
        GTEST_SKIP() << "no MySQL server configured (ORBIT_TEST_MYSQL_PORT)";
    }
};

namespace {

struct TypedResults {
    bool connected = false;
    std::string error;
    size_t rows = 0;
    std::optional<int> i;
    std::optional<double> d;
    std::optional<std::string> s;
    bool n_is_null = false;
    std::optional<int64_t> big;
    std::string by_index;
};

concurrency::Task typed_scenario(database::MysqlClient* db, TypedResults* r, std::promise<void>* done) {
    try {
        co_await db->connect_async();
        r->connected = true;
        database::ResultSet rs = co_await db->query_async(
            "SELECT 42 AS i, 3.5 AS d, 'x' AS s, NULL AS n, 9223372036854775807 AS big");
        r->rows = rs.size();
        if (rs.size() == 1) {
            r->i = rs[0].get_as<int>("i");
            r->d = rs[0].get_as<double>("d");
            r->s = rs[0].get_as<std::string>("s");
            r->n_is_null = rs[0].is_null("n");
            r->big = rs[0].get_as<int64_t>("big");
            r->by_index = rs[0].get(2).value_or("");
        }
    } catch (const std::exception& e) {
        r->error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MysqlClientTest, ConnectsAndReadsTypedValues) {
    Loop loop;
    database::MysqlClient db(loop.proactor, server_config());
    TypedResults r;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { typed_scenario(&db, &r, done); }));

    ASSERT_TRUE(r.connected) << r.error;
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.rows, 1u);
    EXPECT_EQ(r.i, 42);
    EXPECT_EQ(r.d, 3.5);
    EXPECT_EQ(r.s, "x");
    EXPECT_TRUE(r.n_is_null);
    EXPECT_EQ(r.big, 9223372036854775807LL);
    EXPECT_EQ(r.by_index, "x");
}

namespace {

struct WriteResults {
    std::string error;
    uint64_t inserted = 0;
    uint64_t updated = 0;
    uint64_t deleted = 0;
    size_t rows_before_delete = 0;
    std::string names;
    size_t empty_rows = 99;
    std::string roundtrip;
};

concurrency::Task write_scenario(database::MysqlClient* db, std::string tricky, WriteResults* r, std::promise<void>* done) {
    try {
        co_await db->connect_async();
        co_await db->query_async("DROP TABLE IF EXISTS orbit_people");
        co_await db->query_async("CREATE TABLE orbit_people (id INT PRIMARY KEY, name VARCHAR(64), note TEXT)");

        auto ins = co_await db->query_async(
            "INSERT INTO orbit_people (id, name) VALUES (1, 'ada'), (2, 'alan'), (3, 'grace')");
        r->inserted = ins.affected_rows();

        auto upd = co_await db->query_async("UPDATE orbit_people SET name = UPPER(name) WHERE id <= 2");
        r->updated = upd.affected_rows();

        auto all = co_await db->query_async("SELECT name FROM orbit_people ORDER BY id");
        r->rows_before_delete = all.size();
        for (size_t k = 0; k < all.size(); ++k) r->names += all[k].get("name").value_or("?") + ",";

        auto del = co_await db->query_async("DELETE FROM orbit_people WHERE id = 3");
        r->deleted = del.affected_rows();

        auto none = co_await db->query_async("SELECT id FROM orbit_people WHERE id > 100");
        r->empty_rows = none.size();

        // escape() makes arbitrary text safe inside a quoted literal.
        co_await db->query_async("UPDATE orbit_people SET note = '" + db->escape(tricky) + "' WHERE id = 1");
        auto back = co_await db->query_async("SELECT note FROM orbit_people WHERE id = 1");
        if (back.size() == 1) r->roundtrip = back[0].get("note").value_or("<null>");

        co_await db->query_async("DROP TABLE orbit_people");
    } catch (const std::exception& e) {
        r->error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MysqlClientTest, WritesReportAffectedRowsAndEscapeRoundTrips) {
    Loop loop;
    database::MysqlClient db(loop.proactor, server_config());
    using namespace std::string_literals;
    const std::string tricky = "it's a \"quote\", a back\\slash, a NUL \0 and '; DROP TABLE x; --"s;
    WriteResults r;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { write_scenario(&db, tricky, &r, done); }));

    ASSERT_EQ(r.error, "");
    EXPECT_EQ(r.inserted, 3u);
    EXPECT_EQ(r.updated, 2u);
    EXPECT_EQ(r.rows_before_delete, 3u);
    EXPECT_EQ(r.names, "ADA,ALAN,grace,");
    EXPECT_EQ(r.deleted, 1u);
    EXPECT_EQ(r.empty_rows, 0u);
    EXPECT_EQ(r.roundtrip, tricky);
}

namespace {

struct LargeResults {
    std::string error;
    size_t length = 0;
    size_t rows = 0;
};

// Results bigger than a socket read, so the non-blocking calls have to
// wait for the socket and continue more than once.
concurrency::Task large_scenario(database::MysqlClient* db, LargeResults* r, std::promise<void>* done) {
    try {
        co_await db->connect_async();
        auto big = co_await db->query_async("SELECT REPEAT('x', 4000000) AS big_text");
        if (big.size() == 1) r->length = big[0].get("big_text").value_or("").size();
        // 20000 rows from a cross join of digits; a recursive CTE would hit
        // MySQL's default cte_max_recursion_depth (1000).
        const std::string digits = "(SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 "
                                   "UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 "
                                   "UNION ALL SELECT 8 UNION ALL SELECT 9)";
        auto many = co_await db->query_async(
            "SELECT a.n + 10 * b.n + 100 * c.n + 1000 * d.n + 10000 * e.n AS n, REPEAT('y', 50) AS pad FROM " +
            digits + " a, " + digits + " b, " + digits + " c, " + digits + " d, " + digits + " e LIMIT 20000");
        r->rows = many.size();
    } catch (const std::exception& e) {
        r->error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MysqlClientTest, LargeResultsArriveWhole) {
    Loop loop;
    database::MysqlClient db(loop.proactor, server_config());
    LargeResults r;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { large_scenario(&db, &r, done); }));
    ASSERT_EQ(r.error, "");
    EXPECT_EQ(r.length, 4000000u);
    EXPECT_EQ(r.rows, 20000u);
}

namespace {

struct ErrorResults {
    std::string syntax_error;
    std::string missing_table_error;
    std::string after_errors;
};

concurrency::Task error_scenario(database::MysqlClient* db, ErrorResults* r, std::promise<void>* done) {
    try {
        co_await db->connect_async();
    } catch (const std::exception& e) {
        r->syntax_error = std::string("connect failed: ") + e.what();
        done->set_value();
        co_return;
    }
    try {
        co_await db->query_async("SELEKT 1");
    } catch (const std::exception& e) {
        r->syntax_error = e.what();
    }
    try {
        co_await db->query_async("SELECT * FROM orbit_no_such_table");
    } catch (const std::exception& e) {
        r->missing_table_error = e.what();
    }
    // A failed query leaves the connection usable.
    try {
        auto ok = co_await db->query_async("SELECT 'still here' AS v");
        if (ok.size() == 1) r->after_errors = ok[0].get("v").value_or("");
    } catch (const std::exception& e) {
        r->after_errors = std::string("failed: ") + e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MysqlClientTest, QueryErrorsThrowAndTheConnectionSurvives) {
    Loop loop;
    database::MysqlClient db(loop.proactor, server_config());
    ErrorResults r;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { error_scenario(&db, &r, done); }));
    EXPECT_EQ(r.syntax_error.rfind("MySQL Query Error: ", 0), 0u) << r.syntax_error;
    EXPECT_NE(r.syntax_error.find("syntax"), std::string::npos) << r.syntax_error;
    EXPECT_EQ(r.missing_table_error.rfind("MySQL Query Error: ", 0), 0u) << r.missing_table_error;
    EXPECT_NE(r.missing_table_error.find("orbit_no_such_table"), std::string::npos) << r.missing_table_error;
    EXPECT_EQ(r.after_errors, "still here");
}

namespace {

concurrency::Task connect_scenario(database::MysqlClient* db, std::string* error, std::promise<void>* done) {
    try {
        co_await db->connect_async();
    } catch (const std::exception& e) {
        *error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MysqlClientTest, WrongPasswordFailsToConnect) {
    Loop loop;
    auto config = server_config();
    config.password += "-wrong";
    database::MysqlClient db(loop.proactor, config);
    std::string error;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { connect_scenario(&db, &error, done); }));
    EXPECT_EQ(error.rfind("MySQL Connect Error: ", 0), 0u) << error;
    EXPECT_NE(error.find("Access denied"), std::string::npos) << error;
}

TEST_F(MysqlClientTest, UnknownDatabaseFailsToConnect) {
    Loop loop;
    auto config = server_config();
    config.dbname = "orbit_no_such_database";
    database::MysqlClient db(loop.proactor, config);
    std::string error;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { connect_scenario(&db, &error, done); }));
    EXPECT_EQ(error.rfind("MySQL Connect Error: ", 0), 0u) << error;
    EXPECT_NE(error.find("orbit_no_such_database"), std::string::npos) << error;
}

TEST_F(MysqlClientTest, CloseIsIdempotent) {
    Loop loop;
    database::MysqlClient db(loop.proactor, server_config());
    std::string error;
    ASSERT_TRUE(run_scenario([&](std::promise<void>* done) { connect_scenario(&db, &error, done); }));
    ASSERT_EQ(error, "");
    db.close();
    db.close();
    EXPECT_EQ(db.get_mysql(), nullptr);
}

#endif
