#include <gtest/gtest.h>
#include <orbit/websocket/EventRouter.hpp>

using namespace websocket;

TEST(EventRouterTest, InstantiationAndRegistration) {
    EventRouter<EmptySession> router;
    bool handler_called = false;
    
    router.on<std::string>("test_event", [&](auto& /*socket*/, const std::string& data) {
        EXPECT_EQ(data, "hello");
        handler_called = true;
    });
    
    EXPECT_FALSE(handler_called);
}

TEST(EventRouterTest, UntypedHandler) {
    EventRouter<EmptySession> router;
    bool handler_called = false;
    
    router.on("ping", [&](auto& /*socket*/) {
        handler_called = true;
    });
    
    EXPECT_FALSE(handler_called);
}
