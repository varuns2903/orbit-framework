#include <gtest/gtest.h>

#ifndef _WIN32
#include <orbit/network/ConnectionPool.hpp>

#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using network::ConnectionPool;

namespace {

// A connected socket pair: [0] goes in the pool, [1] plays the server.
struct Pair {
    int fds[2]{-1, -1};
    Pair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0); }
    ~Pair() {
        if (fds[1] >= 0) ::close(fds[1]);
    }
    int client() const { return fds[0]; }
    void server_closes() {
        ::close(fds[1]);
        fds[1] = -1;
    }
    void server_sends(const std::string& data) { (void)::write(fds[1], data.data(), data.size()); }
};

bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

// The pool is process-wide; each test uses its own host so they never share
// pooled connections.
std::string host_for(const char* test) { return std::string("pool-test-") + test; }

} // namespace

TEST(ConnectionPoolTest, EmptyPoolHasNothing) {
    auto [fd, ssl] = ConnectionPool::get_instance().acquire(host_for("empty"), 80);
    EXPECT_EQ(fd, -1);
    EXPECT_EQ(ssl, nullptr);
}

TEST(ConnectionPoolTest, ReleasedConnectionIsReusedForTheSameHostAndPort) {
    auto& pool = ConnectionPool::get_instance();
    const std::string host = host_for("reuse");
    Pair p;
    pool.release(host, 80, p.client());

    EXPECT_EQ(pool.acquire(host, 81).first, -1) << "another port is another key";
    EXPECT_EQ(pool.acquire(host + "-other", 80).first, -1) << "another host is another key";
    auto [fd, ssl] = pool.acquire(host, 80);
    EXPECT_EQ(fd, p.client());
    EXPECT_EQ(ssl, nullptr);
    EXPECT_EQ(pool.acquire(host, 80).first, -1) << "a connection is handed out once";
    ::close(fd);
}

TEST(ConnectionPoolTest, NegativeDescriptorIsNotPooled) {
    auto& pool = ConnectionPool::get_instance();
    pool.release(host_for("negative"), 80, -1);
    EXPECT_EQ(pool.acquire(host_for("negative"), 80).first, -1);
}

TEST(ConnectionPoolTest, ConnectionClosedByThePeerIsDiscarded) {
    auto& pool = ConnectionPool::get_instance();
    const std::string host = host_for("closed");
    Pair healthy;
    Pair closed;
    pool.release(host, 80, healthy.client());
    pool.release(host, 80, closed.client()); // most recent: tried first
    closed.server_closes();

    auto [fd, ssl] = pool.acquire(host, 80);
    EXPECT_EQ(fd, healthy.client()) << "the closed one is skipped";
    EXPECT_FALSE(is_open(closed.client())) << "and closed";
    ::close(fd);
}

// Bytes waiting on an idle plain connection (a 408 before the server hangs
// up, say) would be read as the next response.
TEST(ConnectionPoolTest, ConnectionWithUnreadDataIsDiscarded) {
    auto& pool = ConnectionPool::get_instance();
    const std::string host = host_for("stale");
    Pair stale;
    pool.release(host, 80, stale.client());
    stale.server_sends("HTTP/1.1 408 Request Timeout\r\n\r\n");

    EXPECT_EQ(pool.acquire(host, 80).first, -1);
    EXPECT_FALSE(is_open(stale.client()));
}

TEST(ConnectionPoolTest, ConnectionsAreReusedMostRecentFirst) {
    auto& pool = ConnectionPool::get_instance();
    const std::string host = host_for("order");
    Pair a, b;
    pool.release(host, 80, a.client());
    pool.release(host, 80, b.client());
    int first = pool.acquire(host, 80).first;
    int second = pool.acquire(host, 80).first;
    EXPECT_EQ(first, b.client());
    EXPECT_EQ(second, a.client());
    ::close(first);
    ::close(second);
}

// Connections released just now are not stale.
TEST(ConnectionPoolTest, CleanupKeepsRecentConnections) {
    auto& pool = ConnectionPool::get_instance();
    const std::string host = host_for("cleanup");
    Pair p;
    pool.release(host, 80, p.client());
    pool.cleanup_stale_connections();
    int fd = pool.acquire(host, 80).first;
    EXPECT_EQ(fd, p.client());
    ::close(fd);
}

#endif
