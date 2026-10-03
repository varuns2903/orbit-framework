#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/server/Listener.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/network/PlatformSocket.hpp>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace http;

namespace {

// Connects to host:port (IPv4 or IPv6 literal); returns INVALID_SOCKET_FD on failure.
network::socket_t connect_to(const std::string& host, uint16_t port) {
    sockaddr_storage addr{};
    network::socklen_t len = 0;
    int family = AF_INET;
    if (host.find(':') != std::string::npos) {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&addr);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) != 1) return network::INVALID_SOCKET_FD;
        len = sizeof(sockaddr_in6);
        family = AF_INET6;
    } else {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&addr);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) != 1) return network::INVALID_SOCKET_FD;
        len = sizeof(sockaddr_in);
    }
    network::socket_t fd = ::socket(family, SOCK_STREAM, 0);
    if (fd == network::INVALID_SOCKET_FD) return fd;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        network::close_socket(fd);
        return network::INVALID_SOCKET_FD;
    }
    return fd;
}

void set_recv_timeout(network::socket_t fd, int ms) {
#ifdef _WIN32
    DWORD timeout_ms = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Sends one request and reads until the server closes or goes quiet.
std::string request(network::socket_t fd, const std::string& path, bool close, int timeout_ms = 2000) {
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\n" + (close ? "Connection: close\r\n" : "") + "\r\n";
    ::send(fd, req.data(), static_cast<int>(req.size()), 0);
    set_recv_timeout(fd, timeout_ms);
    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        // Keep-alive: stop once one full response ("ok" bodies are short) is in.
        if (!close && out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

std::string body_of(const std::string& response) {
    size_t end = response.find("\r\n\r\n");
    return end == std::string::npos ? "" : response.substr(end + 4);
}

bool ipv6_available() {
    network::socket_t fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd == network::INVALID_SOCKET_FD) return false;
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    network::close_socket(fd);
    return ok;
}

// Starts an App on its own thread and waits until it accepts connections.
struct RunningApp {
    std::unique_ptr<server::App> app;
    std::thread thread;
    std::string probe_host;
    uint16_t port;

    RunningApp(config::ServerConfig cfg, std::string probe)
        : app(std::make_unique<server::App>(cfg)), probe_host(std::move(probe)), port(cfg.port) {
        app->get("/ip", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body(req.client_ip);
            w->send(std::move(res));
        });
        app->get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.set_body("ok");
            w->send(std::move(res));
        });
    }

    void start() {
        thread = std::thread([this] { app->listen(); });
        for (int i = 0; i < 100; ++i) {
            network::socket_t fd = connect_to(probe_host, port);
            if (fd != network::INVALID_SOCKET_FD) {
                std::string res = request(fd, "/ok", true);
                network::close_socket(fd);
                if (res.rfind("HTTP/1.1 200", 0) == 0) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~RunningApp() {
        app->stop();
        // Wake the event loop so it notices the stop flag.
        network::socket_t fd = connect_to(probe_host, port);
        if (fd != network::INVALID_SOCKET_FD) {
            request(fd, "/ok", true, 500);
            network::close_socket(fd);
        }
        if (thread.joinable()) thread.join();
    }
};

} // namespace

// These tests use Listener and raw sockets directly, without an App, so they
// must start the platform networking stack themselves (WSAStartup on Windows).
class ListenerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { network::initialize_platform_networking(); }
    static void TearDownTestSuite() { network::cleanup_platform_networking(); }
};

TEST_F(ListenerTest, BindsLoopbackOnAnEphemeralPort) {
    server::Listener listener("127.0.0.1", 0);
    listener.start();
    ASSERT_NE(listener.port(), 0);

    network::socket_t client = connect_to("127.0.0.1", listener.port());
    ASSERT_NE(client, network::INVALID_SOCKET_FD);

    std::optional<network::Socket> accepted;
    for (int i = 0; i < 100 && !accepted; ++i) {
        accepted = listener.accept_connection();
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(accepted.has_value());
    EXPECT_EQ(network::peer_ip(accepted->fd()), "127.0.0.1");
    network::close_socket(client);
}

TEST_F(ListenerTest, AddressThatIsNotLocalFailsToBind) {
    // 192.0.2.0/24 is reserved for documentation; no host owns it.
    server::Listener listener("192.0.2.1", 0);
    EXPECT_THROW(listener.start(), std::runtime_error);
}

TEST_F(ListenerTest, Ipv6Loopback) {
    if (!ipv6_available()) GTEST_SKIP() << "IPv6 is not available";
    server::Listener listener("::1", 0);
    listener.start();
    network::socket_t client = connect_to("::1", listener.port());
    EXPECT_NE(client, network::INVALID_SOCKET_FD);
    network::close_socket(client);
}

TEST_F(ListenerTest, DualStackAcceptsIpv4Clients) {
    if (!ipv6_available()) GTEST_SKIP() << "IPv6 is not available";
    server::Listener listener("::", 0);
    listener.start();
    network::socket_t client = connect_to("127.0.0.1", listener.port());
    EXPECT_NE(client, network::INVALID_SOCKET_FD);

    std::optional<network::Socket> accepted;
    for (int i = 0; i < 100 && !accepted; ++i) {
        accepted = listener.accept_connection();
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(accepted.has_value());
    // Reported as plain IPv4, not ::ffff:127.0.0.1.
    EXPECT_EQ(network::peer_ip(accepted->fd()), "127.0.0.1");
    network::close_socket(client);
}

TEST_F(ListenerTest, ConfigParsesListenOptions) {
    const char* argv[] = {"app", "--bind", "127.0.0.1", "--backlog", "64", "--max-connections", "5"};
    config::ServerConfig cfg = config::ServerConfig::parse(7, const_cast<char**>(argv));
    EXPECT_EQ(cfg.host, "127.0.0.1");
    EXPECT_EQ(cfg.backlog, 64);
    EXPECT_EQ(cfg.max_connections, 5u);
}

TEST_F(ListenerTest, ManyQueuedConnectionsAreAllServed) {
    config::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 8112;
    RunningApp server(cfg, "127.0.0.1");
    server.start();

    // Queue a burst of connections before reading any response.
    std::vector<network::socket_t> clients;
    for (int i = 0; i < 100; ++i) {
        network::socket_t fd = connect_to("127.0.0.1", cfg.port);
        ASSERT_NE(fd, network::INVALID_SOCKET_FD) << i;
        clients.push_back(fd);
    }
    int ok = 0;
    for (network::socket_t fd : clients) {
        if (body_of(request(fd, "/ok", true)) == "ok") ++ok;
        network::close_socket(fd);
    }
    EXPECT_EQ(ok, 100);
}

TEST_F(ListenerTest, MaxConnectionsQueuesExtraClients) {
    config::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 8113;
    cfg.max_connections = 2;
    RunningApp server(cfg, "127.0.0.1");
    server.start();

    network::socket_t a = connect_to("127.0.0.1", cfg.port);
    network::socket_t b = connect_to("127.0.0.1", cfg.port);
    ASSERT_NE(a, network::INVALID_SOCKET_FD);
    ASSERT_NE(b, network::INVALID_SOCKET_FD);
    // Keep-alive requests: both connections stay open and occupy the slots.
    EXPECT_EQ(body_of(request(a, "/ok", false)), "ok");
    EXPECT_EQ(body_of(request(b, "/ok", false)), "ok");

    // The third connects (the kernel queues it) but is not served yet.
    network::socket_t c = connect_to("127.0.0.1", cfg.port);
    ASSERT_NE(c, network::INVALID_SOCKET_FD);
    EXPECT_EQ(request(c, "/ok", true, 500), "");

    // Freeing a slot lets it through.
    network::close_socket(a);
    set_recv_timeout(c, 3000);
    std::string out;
    char buf[4096];
    while (true) {
        auto n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    EXPECT_EQ(body_of(out), "ok") << out;

    network::close_socket(b);
    network::close_socket(c);
}

TEST_F(ListenerTest, ClientIpOnDualStackServer) {
    if (!ipv6_available()) GTEST_SKIP() << "IPv6 is not available";
    config::ServerConfig cfg;
    cfg.host = "::";
    cfg.port = 8114;
    RunningApp server(cfg, "127.0.0.1");
    server.start();

    network::socket_t v4 = connect_to("127.0.0.1", cfg.port);
    ASSERT_NE(v4, network::INVALID_SOCKET_FD);
    EXPECT_EQ(body_of(request(v4, "/ip", true)), "127.0.0.1");
    network::close_socket(v4);

    network::socket_t v6 = connect_to("::1", cfg.port);
    ASSERT_NE(v6, network::INVALID_SOCKET_FD);
    EXPECT_EQ(body_of(request(v6, "/ip", true)), "::1");
    network::close_socket(v6);
}
