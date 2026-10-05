#include <gtest/gtest.h>
#include <orbit/database/ConnectionPool.hpp>
#include <orbit/database/ResultSet.hpp>

#include <cmath>
#include <cstdint>

using namespace database;

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

database::Row typed_row() {
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
    return database::Row(std::move(vals), cols);
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
