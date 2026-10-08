// What #39 is for: with ORBIT_NO_LEGACY_NAMESPACES (defined for the test
// target), Orbit claims no generic global names, so an application can have
// namespaces called server, http or config of its own next to Orbit's.

#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/config/Config.hpp>

#include <string>

namespace server {
struct App { std::string name = "the application's own server::App"; };
} // namespace server

namespace http {
inline int status_of_my_own() { return 7; }
} // namespace http

namespace config {
constexpr int kMine = 42;
} // namespace config

TEST(NamespaceIsolationTest, ApplicationNamespacesDoNotClashWithOrbit) {
    server::App mine;
    EXPECT_EQ(mine.name, "the application's own server::App");
    EXPECT_EQ(http::status_of_my_own(), 7);
    EXPECT_EQ(config::kMine, 42);

    orbit::config::ServerConfig cfg;
    cfg.port = 0;
    orbit::server::App orbit_app(cfg);
    orbit::http::HttpRequest req;
    req.method = orbit::http::HttpMethod::POST;
    EXPECT_EQ(req.method, orbit::http::HttpMethod::POST);
}
