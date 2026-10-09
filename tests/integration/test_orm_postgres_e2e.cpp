#include <gtest/gtest.h>

#if defined(ORBIT_ENABLE_POSTGRES) && defined(__linux__)
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/QueryBuilder.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/network/EpollProactor.hpp>
#include <orbit/orm/MigrationRunner.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <fstream>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

namespace {

struct Person {
    std::string name;
    std::string note;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Person, name, note)

constexpr int kPgPort = 5440;
const std::string kEvilName = "Robert'); DROP TABLE orm_people; --";

struct Results {
    bool connected = false;
    size_t evil_matches = 0;
    std::string evil_name_read;
    size_t injected_where_matches = 99;
    std::string row_count;
    std::string param_query_value;
    bool bad_operator_threw = false;
};

orbit::concurrency::Task scenario(std::shared_ptr<orbit::database::PostgresClient> db, Results* r, std::promise<void>* done) {
    r->connected = co_await orbit::database::connect_async(db);
    if (r->connected) {
        // Awaiters are named locals: GCC 13 hits an internal compiler error on
        // co_await expressions holding braced lists or non-trivial temporaries.
        using People = orbit::orm::QueryBuilder<orbit::database::PostgresClient, Person>;
        auto create = orbit::database::query_async(db, "CREATE TABLE orm_people (name text, note text);");
        co_await create;

        People insert_evil(db, "orm_people");
        Person evil_person{kEvilName, "n1"};
        auto insert1 = insert_evil.insert_async(evil_person);
        co_await insert1;

        People insert_alice(db, "orm_people");
        Person alice{"alice", "n2"};
        auto insert2 = insert_alice.insert_async(alice);
        co_await insert2;

        People find_evil(db, "orm_people");
        find_evil.where(orbit::orm::Col("name") == kEvilName);
        auto get_evil = find_evil.get_async();
        std::vector<Person> evil = co_await get_evil;
        r->evil_matches = evil.size();
        if (!evil.empty()) r->evil_name_read = evil[0].name;

        // With string splicing this became "note = 'n2' OR '1'='1'" and matched every row.
        People find_injected(db, "orm_people");
        find_injected.where("note", "=", "n2' OR '1'='1");
        auto get_injected = find_injected.get_async();
        std::vector<Person> injected = co_await get_injected;
        r->injected_where_matches = injected.size();

        try {
            People bad(db, "orm_people");
            bad.where("note", "= 'x' OR 1=1 --", "y");
        } catch (const std::invalid_argument&) {
            r->bad_operator_threw = true;
        }

        auto count_query = orbit::database::query_async(db, "SELECT count(*) AS n FROM orm_people;");
        orbit::database::ResultSet count = co_await count_query;
        if (count.size() == 1) r->row_count = count[0].get(0).value_or("");

        orbit::orm::Params params;
        params.emplace_back(std::string("it's; fine"));
        auto param_query = orbit::database::query_async(db, "SELECT $1::text AS v;", params);
        orbit::database::ResultSet param = co_await param_query;
        if (param.size() == 1) r->param_query_value = param[0].get(0).value_or("");
    }
    done->set_value();
}

struct Stock {
    std::string name;
    int qty = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Stock, name, qty)

struct CrudResults {
    bool connected = false;
    std::vector<std::string> page;
    uint64_t count_over_2 = 0;
    uint64_t updated = 0;
    int c_qty_after_update = -1;
    uint64_t removed = 0;
    uint64_t remaining = 0;
    bool unconditional_update_threw = false;
    uint64_t updated_all = 0;
    uint64_t nonzero_after_all = 99;
};

orbit::concurrency::Task crud_scenario(std::shared_ptr<orbit::database::PostgresClient> db, CrudResults* r, std::promise<void>* done) {
    using Stocks = orbit::orm::QueryBuilder<orbit::database::PostgresClient, Stock>;
    r->connected = co_await orbit::database::connect_async(db);
    if (r->connected) {
        // "user" is a reserved word: only works because the ORM quotes it.
        auto create = orbit::database::query_async(db, "CREATE TABLE \"user\" (name text, qty int);");
        co_await create;
        const char* names[] = {"a", "b", "c", "d", "e"};
        for (int i = 0; i < 5; ++i) {
            Stocks ins(db, "user");
            Stock s{names[i], i}; // qty 0..4
            auto insert = ins.insert_async(s);
            co_await insert;
        }

        Stocks page(db, "user");
        page.order_by("qty", orbit::orm::Order::Desc).limit(2).offset(1);
        auto get_page = page.get_async();
        std::vector<Stock> rows = co_await get_page;
        for (const auto& s : rows) r->page.push_back(s.name);

        Stocks over2(db, "user");
        over2.where(orbit::orm::Col("qty") > 2);
        auto count = over2.count_async();
        r->count_over_2 = co_await count;

        Stocks upd(db, "user");
        upd.where(orbit::orm::Col("name") == "c");
        nlohmann::json changes = {{"qty", 100}};
        auto update = upd.update_async(changes);
        r->updated = co_await update;
        Stocks find_c(db, "user");
        find_c.where(orbit::orm::Col("name") == "c");
        auto get_c = find_c.get_async();
        std::vector<Stock> c = co_await get_c;
        if (c.size() == 1) r->c_qty_after_update = c[0].qty;

        Stocks del(db, "user");
        del.where(orbit::orm::Col("qty") < 2);
        auto remove = del.remove_async();
        r->removed = co_await remove;
        Stocks left(db, "user");
        auto count_left = left.count_async();
        r->remaining = co_await count_left;

        try {
            Stocks oops(db, "user");
            nlohmann::json zero = {{"qty", 0}};
            oops.update_async(zero);
        } catch (const std::logic_error&) {
            r->unconditional_update_threw = true;
        }
        Stocks everything(db, "user");
        everything.all();
        nlohmann::json zero = {{"qty", 0}};
        auto update_all = everything.update_async(zero);
        r->updated_all = co_await update_all;
        Stocks nonzero(db, "user");
        nonzero.where(orbit::orm::Col("qty") != 0);
        auto count_nonzero = nonzero.count_async();
        r->nonzero_after_all = co_await count_nonzero;
    }
    done->set_value();
}

} // namespace

class OrmPostgresTest : public ::testing::Test {
protected:
    static std::string data_dir, socket_dir;
    static bool available;

    static void SetUpTestSuite() {
        if (std::system("command -v initdb >/dev/null 2>&1 && command -v pg_ctl >/dev/null 2>&1") != 0) return;
        auto base = std::filesystem::temp_directory_path() / ("orbit_pg_test_" + std::to_string(::getpid()));
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

std::string OrmPostgresTest::data_dir;
std::string OrmPostgresTest::socket_dir;
bool OrmPostgresTest::available = false;

TEST_F(OrmPostgresTest, ValuesAreBoundNotSpliced) {
    orbit::network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });

    auto db = std::make_shared<orbit::database::PostgresClient>(
        &proactor, "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres");
    Results r;
    std::promise<void> done;
    auto finished = done.get_future();
    scenario(db, &r, &done);
    bool completed = finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready;

    running = false;
    loop.join();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.evil_matches, 1u);
    EXPECT_EQ(r.evil_name_read, kEvilName);
    EXPECT_EQ(r.injected_where_matches, 0u);
    EXPECT_TRUE(r.bad_operator_threw);
    EXPECT_EQ(r.row_count, "2"); // table still there, nothing extra inserted
    EXPECT_EQ(r.param_query_value, "it's; fine");
}

namespace {

class MigrationWriter : public orbit::http::ResponseWriter {
public:
    std::promise<std::pair<int, std::string>> result;
    void send(orbit::http::HttpResponse&& r) override { result.set_value({static_cast<int>(r.status_code), r.body}); }
    void send_headers(orbit::http::HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(Interceptor) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

struct ErrorResults {
    bool connected = false;
    bool missing_ok = true;
    std::string missing_error;
    bool null_is_nullopt = false;
    std::string empty_value = "unset";
    nlohmann::json row_json;
    std::string tracked;
    bool mig_b_exists = true;
};

orbit::concurrency::Task error_scenario(std::shared_ptr<orbit::database::PostgresClient> db, ErrorResults* r, std::promise<void>* done) {
    r->connected = co_await orbit::database::connect_async(db);
    if (r->connected) {
        auto missing = co_await orbit::database::query_async(db, "SELECT * FROM no_such_table;");
        r->missing_ok = missing.ok();
        r->missing_error = missing.error();

        auto nulls = co_await orbit::database::query_async(db, "SELECT NULL::text AS a, ''::text AS b;");
        if (nulls.ok() && nulls.size() == 1) {
            r->null_is_nullopt = !nulls[0].get(0).has_value();
            r->empty_value = nulls[0].get(1).value_or("nullopt");
            r->row_json = nulls[0].to_json();
        }
    }
    done->set_value();
}

orbit::concurrency::Task inspect_migrations(std::shared_ptr<orbit::database::PostgresClient> db, ErrorResults* r, std::promise<void>* done) {
    auto tracked = co_await orbit::database::query_async(db, "SELECT string_agg(version, ',' ORDER BY version) FROM orbit_migrations;");
    if (tracked.ok() && tracked.size() == 1) r->tracked = tracked[0].get(0).value_or("");
    auto exists = co_await orbit::database::query_async(db, "SELECT to_regclass('mig_b') IS NOT NULL;");
    if (exists.ok() && exists.size() == 1) r->mig_b_exists = exists[0].get(0).value_or("") == "t";
    done->set_value();
}

} // namespace

TEST_F(OrmPostgresTest, ErrorsNullsAndTransactionalMigrations) {
    orbit::network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });
    auto db = std::make_shared<orbit::database::PostgresClient>(
        &proactor, "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres");

    ErrorResults r;
    {
        std::promise<void> done;
        auto f = done.get_future();
        error_scenario(db, &r, &done);
        ASSERT_EQ(f.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    }
    ASSERT_TRUE(r.connected);
    EXPECT_FALSE(r.missing_ok);
    EXPECT_NE(r.missing_error.find("no_such_table"), std::string::npos) << r.missing_error;
    EXPECT_TRUE(r.null_is_nullopt);
    EXPECT_EQ(r.empty_value, "");
    EXPECT_TRUE(r.row_json["a"].is_null());
    EXPECT_EQ(r.row_json["b"], "");

    // Migrations: 001 succeeds, 002 fails half-way and must leave nothing behind.
    auto dir = std::filesystem::temp_directory_path() / ("orbit_migrations_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "001_a.sql") << "CREATE TABLE mig_a (x int);";
    std::ofstream(dir / "002_b.sql") << "CREATE TABLE mig_b (x int); SELECT * FROM does_not_exist;";

    auto run = [&]() {
        auto writer = std::make_shared<MigrationWriter>();
        auto f = writer->result.get_future();
        orbit::orm::MigrationRunner<orbit::database::PostgresClient>::run_migrations(db, dir.string(), writer);
        EXPECT_EQ(f.wait_for(std::chrono::seconds(20)), std::future_status::ready);
        return f.get();
    };

    auto first = run();
    EXPECT_EQ(first.first, 500) << first.second;
    {
        std::promise<void> done;
        auto f = done.get_future();
        inspect_migrations(db, &r, &done);
        ASSERT_EQ(f.wait_for(std::chrono::seconds(20)), std::future_status::ready);
    }
    EXPECT_EQ(r.tracked, "001_a.sql"); // 002 was not recorded as applied
    EXPECT_FALSE(r.mig_b_exists);      // and its partial work was rolled back

    std::ofstream(dir / "002_b.sql", std::ios::trunc) << "CREATE TABLE mig_b (x int);";
    auto second = run();
    EXPECT_EQ(second.first, 200);
    EXPECT_EQ(second.second, "Successfully applied 1 migrations.");

    std::filesystem::remove_all(dir);
    running = false;
    loop.join();
}
namespace {

orbit::concurrency::Task run_directly(std::shared_ptr<orbit::database::PostgresClient> db, std::string dir,
                                      orbit::orm::MigrationResult* out, std::promise<void>* done) {
    auto connecting = orbit::database::connect_async(db);
    if (co_await connecting) {
        auto running = orbit::orm::MigrationRunner<orbit::database::PostgresClient>::run(db, dir);
        *out = co_await running;
        // Leave orbit_migrations as the other tests expect it.
        auto cleanup = orbit::database::query_async(
            db, "DELETE FROM orbit_migrations WHERE version LIKE '%_startup_%'; DROP TABLE IF EXISTS startup_a;");
        co_await cleanup;
    }
    done->set_value();
}

} // namespace

// Migrations without an HTTP request (#195): run() as a coroutine, and
// migrate_sync() for start-up code, with its own event loop.
TEST_F(OrmPostgresTest, MigrationsRunWithoutAResponseWriter) {
    const std::string info = "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres";
    auto dir = std::filesystem::temp_directory_path() / ("orbit_startup_migrations_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "101_startup_a.sql") << "CREATE TABLE startup_a (x int);";
    std::ofstream(dir / "102_startup_b.sql") << "INSERT INTO startup_a VALUES (1);";

    orbit::orm::MigrationResult first = orbit::orm::migrate_sync(info, dir.string());
    EXPECT_TRUE(first.ok()) << first.error;
    EXPECT_EQ(first.applied, (std::vector<std::string>{"101_startup_a.sql", "102_startup_b.sql"}));
    EXPECT_EQ(first.summary(), "Successfully applied 2 migrations.");

    orbit::orm::MigrationResult again = orbit::orm::migrate_sync(info, dir.string());
    EXPECT_TRUE(again.ok()) << again.error;
    EXPECT_TRUE(again.applied.empty());
    EXPECT_EQ(again.summary(), "Database is up to date");

    orbit::orm::MigrationResult missing = orbit::orm::migrate_sync(info, (dir / "nope").string());
    EXPECT_TRUE(missing.ok()) << missing.error;
    EXPECT_TRUE(missing.directory_missing);

    // Nothing listens on this port.
    orbit::orm::MigrationResult unreachable = orbit::orm::migrate_sync(
        "host=" + socket_dir + " port=1 user=postgres dbname=postgres connect_timeout=2", dir.string(),
        std::chrono::seconds(10));
    EXPECT_FALSE(unreachable.ok());
    EXPECT_EQ(unreachable.error, "Could not connect to the database");

    // The coroutine form, on an event loop the caller runs.
    std::ofstream(dir / "103_startup_c.sql") << "SELECT * FROM no_such_table_here;";
    {
        orbit::network::EpollProactor proactor;
        std::atomic<bool> running{true};
        std::thread loop([&] {
            while (running) proactor.run_once(50);
        });
        auto db = std::make_shared<orbit::database::PostgresClient>(&proactor, info);
        orbit::orm::MigrationResult result;
        std::promise<void> done;
        auto finished = done.get_future();
        run_directly(db, dir.string(), &result, &done);
        bool completed = finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
        running = false;
        loop.join();
        ASSERT_TRUE(completed);
        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(result.applied.empty());
        EXPECT_NE(result.error.find("103_startup_c.sql failed and was rolled back"), std::string::npos) << result.error;
    }

    std::filesystem::remove_all(dir);
}

TEST_F(OrmPostgresTest, UpdateDeleteOrderLimitCountAndQuoting) {
    orbit::network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });

    auto db = std::make_shared<orbit::database::PostgresClient>(
        &proactor, "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres");
    CrudResults r;
    std::promise<void> done;
    auto finished = done.get_future();
    crud_scenario(db, &r, &done);
    bool completed = finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready;

    running = false;
    loop.join();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(r.connected);
    EXPECT_EQ(r.page, (std::vector<std::string>{"d", "c"})); // qty 4,3,2,... skip 1, take 2
    EXPECT_EQ(r.count_over_2, 2u);
    EXPECT_EQ(r.updated, 1u);
    EXPECT_EQ(r.c_qty_after_update, 100);
    EXPECT_EQ(r.removed, 2u); // qty 0 and 1
    EXPECT_EQ(r.remaining, 3u);
    EXPECT_TRUE(r.unconditional_update_threw);
    EXPECT_EQ(r.updated_all, 3u);
    EXPECT_EQ(r.nonzero_after_all, 0u);
}

// ---- primary keys, RETURNING and errors (#187)

namespace {

struct Ticket {
    int id = 0;
    std::string title;
    int qty = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Ticket, id, title, qty)

struct KeyResults {
    bool connected = false;
    Ticket created;
    uint64_t second_insert_rows = 0;
    size_t rows_after = 0;
    std::string title_after_update;
    int id_after_update = -1;
    bool insert_error_thrown = false;
    std::string insert_error;
    bool bad_row_thrown = false;
};

orbit::concurrency::Task key_scenario(std::shared_ptr<orbit::database::PostgresClient> db, KeyResults* r,
                                      std::promise<void>* done) {
    using Tickets = orbit::orm::QueryBuilder<orbit::database::PostgresClient, Ticket>;
    r->connected = co_await orbit::database::connect_async(db);
    if (r->connected) {
        auto create_table = orbit::database::query_async(
            db, "CREATE TABLE orm_tickets (id SERIAL PRIMARY KEY, title text NOT NULL, qty int NOT NULL DEFAULT 0);");
        co_await create_table;

        // id 0 is "unset": the database generates it, RETURNING gives it back.
        Tickets t1(db, "orm_tickets");
        auto creating = t1.create_async(Ticket{0, "42", 3});
        r->created = co_await creating;

        // A second id-0 insert used to collide with the first.
        Tickets t2(db, "orm_tickets");
        auto inserting = t2.insert_async(Ticket{0, "second", 1});
        r->second_insert_rows = co_await inserting;

        Tickets t3(db, "orm_tickets");
        auto all = t3.get_async();
        auto rows = co_await all;
        r->rows_after = rows.size();

        // Updating from a model with id 0 must not rewrite the key.
        Tickets t4(db, "orm_tickets");
        t4.where(orbit::orm::Col("id") == r->created.id);
        auto updating = t4.update_async(Ticket{0, "renamed", 9});
        co_await updating;
        Tickets t5(db, "orm_tickets");
        t5.where(orbit::orm::Col("title") == std::string("renamed"));
        auto reread = t5.get_async();
        auto renamed = co_await reread;
        if (!renamed.empty()) {
            r->title_after_update = renamed[0].title;
            r->id_after_update = renamed[0].id;
        }

        // A failing INSERT throws instead of reporting 0 rows.
        try {
            Tickets missing(db, "orm_no_such_table");
            auto failing = missing.insert_async(Ticket{0, "x", 1});
            co_await failing;
        } catch (const orbit::orm::DatabaseError& e) {
            r->insert_error_thrown = true;
            r->insert_error = e.what();
        }

        // NULL in a non-optional field is reported, not a crash.
        auto nullable = orbit::database::query_async(db, "CREATE TABLE orm_nullable (id int, title text, qty int);");
        co_await nullable;
        auto add_null = orbit::database::query_async(db, "INSERT INTO orm_nullable VALUES (NULL, 't', 1);");
        co_await add_null;
        try {
            Tickets n(db, "orm_nullable");
            auto reading = n.get_async();
            co_await reading;
        } catch (const orbit::orm::DatabaseError&) {
            r->bad_row_thrown = true;
        }
    }
    done->set_value();
}

} // namespace

TEST_F(OrmPostgresTest, CreateReturnsTheKeyAndErrorsThrow) {
    orbit::network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });
    auto db = std::make_shared<orbit::database::PostgresClient>(
        &proactor, "host=" + socket_dir + " port=" + std::to_string(kPgPort) + " user=postgres dbname=postgres");

    KeyResults r;
    std::promise<void> done;
    auto finished = done.get_future();
    key_scenario(db, &r, &done);
    bool completed = finished.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    running = false;
    loop.join();

    ASSERT_TRUE(completed);
    ASSERT_TRUE(r.connected);
    EXPECT_GT(r.created.id, 0) << "RETURNING should give the generated key";
    EXPECT_EQ(r.created.title, "42") << "a numeric-looking string stays a string";
    EXPECT_EQ(r.created.qty, 3);
    EXPECT_EQ(r.second_insert_rows, 1u);
    EXPECT_EQ(r.rows_after, 2u);
    EXPECT_EQ(r.title_after_update, "renamed");
    EXPECT_EQ(r.id_after_update, r.created.id) << "update_async(model) must keep the key";
    EXPECT_TRUE(r.insert_error_thrown);
    EXPECT_NE(r.insert_error.find("orm_no_such_table"), std::string::npos) << r.insert_error;
    EXPECT_TRUE(r.bad_row_thrown);
}


#endif
