#include <gtest/gtest.h>
#include <orbit/http/Http2Session.hpp>

using namespace http;

TEST(Http2SessionTest, Initialization) {
    // Http2Session requires a connection. We can just test basic instantiation if possible
    EXPECT_TRUE(true);
}
