#include <gtest/gtest.h>

#if defined(ORBIT_ENABLE_MONGODB)
#include <orbit/database/MongoClient.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

// MongoClient against a real server. CI starts one in a service container
// and sets ORBIT_TEST_MONGODB_URI (e.g. mongodb://127.0.0.1:27017). Without
// it the tests skip, unless ORBIT_REQUIRE_MONGODB_TESTS is set, in which
// case they fail.

namespace {

std::string server_uri() {
    const char* uri = std::getenv("ORBIT_TEST_MONGODB_URI");
    return uri ? uri : "";
}

// A collection no other test (or earlier run) has written to.
orbit::database::MongoClient::Config config_for(const std::string& collection) {
    orbit::database::MongoClient::Config c;
    c.uri = server_uri();
    c.dbname = "orbit_test";
    c.collection_name = collection + "_" + std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
    return c;
}

// Signals that a scenario finished. Not std::promise: on MSVC <future>
// declares a "concurrency" namespace that collides with Orbit's.
class Done {
public:
    void set_value() {
        std::lock_guard<std::mutex> lock(m_);
        done_ = true;
        cv_.notify_all();
    }
    bool wait_for(std::chrono::seconds limit) {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, limit, [this] { return done_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
};

template <typename F>
bool run_scenario(F&& start, std::chrono::seconds limit = std::chrono::seconds(30)) {
    Done done;
    start(&done);
    return done.wait_for(limit);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

class MongoClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!server_uri().empty()) return;
        const char* required = std::getenv("ORBIT_REQUIRE_MONGODB_TESTS");
        if (required && *required) FAIL() << "ORBIT_REQUIRE_MONGODB_TESTS is set but ORBIT_TEST_MONGODB_URI is not";
        GTEST_SKIP() << "no MongoDB server configured (ORBIT_TEST_MONGODB_URI)";
    }
    orbit::concurrency::ThreadPool pool{2};
};

namespace {

struct CrudResults {
    std::string error;
    bool inserted_all = true;
    size_t all = 0;
    size_t matching = 0;
    std::string matched_doc;
    size_t none = 99;
};

orbit::concurrency::Task crud_scenario(orbit::database::MongoClient* db, CrudResults* r, Done* done) {
    try {
        for (const char* doc : {R"({"name": "ada", "year": 1815})",
                                R"({"name": "alan", "year": 1912})",
                                R"({"name": "grace", "year": 1906, "tags": ["navy", "cobol"]})"}) {
            r->inserted_all = (co_await db->insert_async(doc)) && r->inserted_all;
        }
        r->all = (co_await db->find_async("{}")).documents.size();
        auto modern = co_await db->find_async(R"({"year": {"$gt": 1900}})");
        r->matching = modern.documents.size();
        auto grace = co_await db->find_async(R"({"name": "grace"})");
        if (grace.documents.size() == 1) r->matched_doc = grace.documents[0];
        r->none = (co_await db->find_async(R"({"name": "nobody"})")).documents.size();
    } catch (const std::exception& e) {
        r->error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MongoClientTest, InsertsAndFindsByFilter) {
    orbit::database::MongoClient db(pool, config_for("crud"));
    CrudResults r;
    ASSERT_TRUE(run_scenario([&](Done* done) { crud_scenario(&db, &r, done); }));
    ASSERT_EQ(r.error, "");
    EXPECT_TRUE(r.inserted_all);
    EXPECT_EQ(r.all, 3u);
    EXPECT_EQ(r.matching, 2u);
    EXPECT_EQ(r.none, 0u);
    // Documents come back as canonical extended JSON.
    EXPECT_TRUE(contains(r.matched_doc, R"("name" : "grace")")) << r.matched_doc;
    EXPECT_TRUE(contains(r.matched_doc, R"("year" : { "$numberInt" : "1906" })")) << r.matched_doc;
    EXPECT_TRUE(contains(r.matched_doc, R"("navy", "cobol")")) << r.matched_doc;
    EXPECT_TRUE(contains(r.matched_doc, R"("_id" : { "$oid" : ")")) << r.matched_doc;
}

namespace {

struct ErrorResults {
    std::string bad_filter;
    std::string bad_document;
    std::string duplicate;
    std::string bad_operator;
};

orbit::concurrency::Task error_scenario(orbit::database::MongoClient* db, ErrorResults* r, Done* done) {
    try {
        co_await db->find_async("{not json");
    } catch (const std::exception& e) {
        r->bad_filter = e.what();
    }
    try {
        co_await db->insert_async("[1, 2");
    } catch (const std::exception& e) {
        r->bad_document = e.what();
    }
    try {
        co_await db->insert_async(R"({"_id": "same"})");
        co_await db->insert_async(R"({"_id": "same"})");
    } catch (const std::exception& e) {
        r->duplicate = e.what();
    }
    try {
        co_await db->find_async(R"({"year": {"$frobnicate": 1}})");
    } catch (const std::exception& e) {
        r->bad_operator = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MongoClientTest, ErrorsAreThrownWithTheirCause) {
    orbit::database::MongoClient db(pool, config_for("errors"));
    ErrorResults r;
    ASSERT_TRUE(run_scenario([&](Done* done) { error_scenario(&db, &r, done); }));
    EXPECT_EQ(r.bad_filter.rfind("BSON Parse Error: ", 0), 0u) << r.bad_filter;
    EXPECT_EQ(r.bad_document.rfind("BSON Parse Error: ", 0), 0u) << r.bad_document;
    EXPECT_EQ(r.duplicate.rfind("MongoDB Insert Error: ", 0), 0u) << r.duplicate;
    EXPECT_TRUE(contains(r.duplicate, "duplicate key")) << r.duplicate;
    EXPECT_EQ(r.bad_operator.rfind("MongoDB Cursor Error: ", 0), 0u) << r.bad_operator;
    EXPECT_TRUE(contains(r.bad_operator, "$frobnicate")) << r.bad_operator;
}

TEST_F(MongoClientTest, InvalidUriIsRejectedAtConstruction) {
    orbit::database::MongoClient::Config c;
    c.uri = "not-a-mongodb-uri";
    try {
        orbit::database::MongoClient db(pool, c);
        FAIL() << "constructed a client from an invalid URI";
    } catch (const std::runtime_error& e) {
        EXPECT_EQ(std::string(e.what()).rfind("Failed to parse MongoDB URI: ", 0), 0u) << e.what();
    }
}

namespace {

orbit::concurrency::Task find_scenario(orbit::database::MongoClient* db, std::string* error, Done* done) {
    try {
        co_await db->find_async("{}");
    } catch (const std::exception& e) {
        *error = e.what();
    }
    done->set_value();
}

} // namespace

TEST_F(MongoClientTest, UnreachableServerFailsTheQuery) {
    orbit::database::MongoClient::Config c;
    // Port 1 refuses connections; the short selection timeout keeps it quick.
    c.uri = "mongodb://127.0.0.1:1/?serverSelectionTimeoutMS=300&connectTimeoutMS=300";
    c.dbname = "orbit_test";
    c.collection_name = "unreachable";
    orbit::database::MongoClient db(pool, c);
    std::string error;
    ASSERT_TRUE(run_scenario([&](Done* done) { find_scenario(&db, &error, done); }));
    EXPECT_EQ(error.rfind("MongoDB Cursor Error: ", 0), 0u) << error;
}

// mongoc_init/mongoc_cleanup are reference counted across clients: the
// library stays initialised while any client exists.
TEST_F(MongoClientTest, ClientsCanComeAndGo) {
    for (int round = 0; round < 3; ++round) {
        auto first = std::make_unique<orbit::database::MongoClient>(pool, config_for("lifecycle"));
        auto second = std::make_unique<orbit::database::MongoClient>(pool, config_for("lifecycle"));
        first.reset();
        CrudResults r;
        ASSERT_TRUE(run_scenario([&](Done* done) { crud_scenario(second.get(), &r, done); }));
        EXPECT_EQ(r.error, "") << "round " << round;
        EXPECT_EQ(r.all, 3u) << "round " << round;
    }
}

#endif
