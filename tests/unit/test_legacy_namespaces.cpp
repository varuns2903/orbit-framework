// The 1.x top-level names (server::, http::, ...) still work as aliases of
// orbit::, unless ORBIT_NO_LEGACY_NAMESPACES is defined. The test target
// defines it for every other file, so turn the aliases back on here.
#undef ORBIT_NO_LEGACY_NAMESPACES

#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/utils/Logger.hpp>

#include <type_traits>

static_assert(std::is_same_v<server::App, orbit::server::App>);
static_assert(std::is_same_v<http::HttpRequest, orbit::http::HttpRequest>);
static_assert(std::is_same_v<config::ServerConfig, orbit::config::ServerConfig>);
static_assert(std::is_same_v<routing::Router, orbit::routing::Router>);

TEST(LegacyNamespacesTest, OldNamesStillWork) {
    config::ServerConfig cfg;
    cfg.port = 0;
    server::App app(cfg);
    app.use(middleware::cors());
    http::HttpRequest req;
    req.method = http::HttpMethod::GET;
    EXPECT_EQ(req.method, orbit::http::HttpMethod::GET);
}

// Code that combines the aliases with using-directives must not become
// ambiguous: both names denote the same namespace.
TEST(LegacyNamespacesTest, AliasesAndUsingDirectivesMix) {
    using namespace orbit;
    http::HttpRequest req; // via the alias, or orbit::http through the directive
    req.uri = "/x";
    EXPECT_EQ(req.uri, "/x");
}
