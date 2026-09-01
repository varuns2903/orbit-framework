#include <gtest/gtest.h>
#include <orbit/http/WebSocket.hpp>

class WebSocketTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(WebSocketTest, ValidHandshakeGeneration) {
    // Standard example from RFC 6455
    std::string client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    
    std::string generated_accept = http::websocket::Handshake::generate_accept_key(client_key);
    
    EXPECT_EQ(generated_accept, expected_accept);
}
