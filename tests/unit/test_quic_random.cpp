#include <gtest/gtest.h>

#ifdef ORBIT_ENABLE_HTTP3
#include <orbit/server/QuicConnection.hpp>

#include <array>
#include <set>
#include <string>

TEST(QuicRandomTest, BytesDifferAcrossCalls) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        std::array<uint8_t, 16> buf{};
        ASSERT_TRUE(server::detail::quic_random_bytes(buf.data(), buf.size()));
        seen.emplace(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(QuicRandomTest, NotTheUnseededRandSequence) {
    // rand() without srand() yields the same bytes in every process; the
    // first 8 of them were used as the server's connection ID.
    std::array<uint8_t, 8> libc{};
    srand(1);
    for (auto& b : libc) b = static_cast<uint8_t>(rand() % 256);
    std::array<uint8_t, 8> ours{};
    ASSERT_TRUE(server::detail::quic_random_bytes(ours.data(), ours.size()));
    EXPECT_NE(ours, libc);
}

TEST(QuicRandomTest, ZeroLengthIsFine) {
    EXPECT_TRUE(server::detail::quic_random_bytes(nullptr, 0));
}
#endif // ORBIT_ENABLE_HTTP3
