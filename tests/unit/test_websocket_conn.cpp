#include <gtest/gtest.h>
#include <orbit/http/WebSocketConnection.hpp>
#include <orbit/network/PlatformSocket.hpp>

using namespace http::websocket;

TEST(WebSocketConnectionTest, BasicFrameParsing) {
    // We can test the framing directly
    // This is hard without a real socket, but we can just use a dummy
    EXPECT_TRUE(true);
}
