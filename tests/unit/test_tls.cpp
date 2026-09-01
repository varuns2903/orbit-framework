#include <gtest/gtest.h>
#include <orbit/network/TlsContext.hpp>
#include <fstream>
#include <filesystem>

using namespace network;

TEST(TlsContextTest, InvalidCertsThrow) {
    EXPECT_THROW({
        TlsContext ctx("invalid.crt", "invalid.key", config::HttpVersion::Http1_1);
    }, std::runtime_error);
}

TEST(TlsContextTest, EmptyCertsThrow) {
    EXPECT_THROW({
        TlsContext ctx("", "", config::HttpVersion::Http1_1);
    }, std::runtime_error);
}
