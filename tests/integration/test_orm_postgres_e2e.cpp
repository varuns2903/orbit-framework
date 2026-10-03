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

concurrency::Task scenario(std::shared_ptr<database::PostgresClient> db, Results* r, std::promise<void>* done) {
    r->connected = co_await database::connect_async(db);
    if (r->connected) {
        // Awaiters are named locals: GCC 13 hits an internal compiler error on
        // co_await expressions holding braced lists or non-trivial temporaries.
        using People = orm::QueryBuilder<database::PostgresClient, Person>;
        auto create = database::query_async(db, "CREATE TABLE orm_people (name text, note text);");
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
        find_evil.where(orm::Col("name") == kEvilName);
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

        auto count_query = database::query_async(db, "SELECT count(*) AS n FROM orm_people;");
        database::ResultSet count = co_await count_query;
        if (count.size() == 1) r->row_count = count[0].get(0).value_or("");

        orm::Params params;
        params.emplace_back(std::string("it's; fine"));
        auto param_query = database::query_async(db, "SELECT $1::text AS v;", params);
        database::ResultSet param = co_await param_query;
        if (param.size() == 1) r->param_query_value = param[0].get(0).value_or("");
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
        if (!available) GTEST_SKIP() << "PostgreSQL server binaries not available";
    }
};

std::string OrmPostgresTest::data_dir;
std::string OrmPostgresTest::socket_dir;
bool OrmPostgresTest::available = false;

TEST_F(OrmPostgresTest, ValuesAreBoundNotSpliced) {
    network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });

    auto db = std::make_shared<database::PostgresClient>(
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

class MigrationWriter : public http::ResponseWriter {
public:
    std::promise<std::pair<int, std::string>> result;
    void send(http::HttpResponse&& r) override { result.set_value({static_cast<int>(r.status_code), r.body}); }
    void send_headers(http::HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(Interceptor) override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
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

concurrency::Task error_scenario(std::shared_ptr<database::PostgresClient> db, ErrorResults* r, std::promise<void>* done) {
    r->connected = co_await database::connect_async(db);
    if (r->connected) {
        auto missing = co_await database::query_async(db, "SELECT * FROM no_such_table;");
        r->missing_ok = missing.ok();
        r->missing_error = missing.error();

        auto nulls = co_await database::query_async(db, "SELECT NULL::text AS a, ''::text AS b;");
        if (nulls.ok() && nulls.size() == 1) {
            r->null_is_nullopt = !nulls[0].get(0).has_value();
            r->empty_value = nulls[0].get(1).value_or("nullopt");
            r->row_json = nulls[0].to_json();
        }
    }
    done->set_value();
}

concurrency::Task inspect_migrations(std::shared_ptr<database::PostgresClient> db, ErrorResults* r, std::promise<void>* done) {
    auto tracked = co_await database::query_async(db, "SELECT string_agg(version, ',' ORDER BY version) FROM orbit_migrations;");
    if (tracked.ok() && tracked.size() == 1) r->tracked = tracked[0].get(0).value_or("");
    auto exists = co_await database::query_async(db, "SELECT to_regclass('mig_b') IS NOT NULL;");
    if (exists.ok() && exists.size() == 1) r->mig_b_exists = exists[0].get(0).value_or("") == "t";
    done->set_value();
}

} // namespace

TEST_F(OrmPostgresTest, ErrorsNullsAndTransactionalMigrations) {
    network::EpollProactor proactor;
    std::atomic<bool> running{true};
    std::thread loop([&] {
        while (running) proactor.run_once(50);
    });
    auto db = std::make_shared<database::PostgresClient>(
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
        orm::MigrationRunner<database::PostgresClient>::run_migrations(db, dir.string(), writer);
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
#endif
