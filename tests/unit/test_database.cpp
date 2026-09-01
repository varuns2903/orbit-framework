#include <gtest/gtest.h>
#include <orbit/database/ConnectionPool.hpp>

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
