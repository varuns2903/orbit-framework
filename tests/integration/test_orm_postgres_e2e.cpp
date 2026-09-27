#include <gtest/gtest.h>

#if defined(ORBIT_ENABLE_POSTGRES) && defined(__linux__)
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/QueryBuilder.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/network/EpollProactor.hpp>

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
#endif
