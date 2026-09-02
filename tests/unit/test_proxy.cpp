#include <gtest/gtest.h>
#include <orbit/middleware/Proxy.hpp>

using namespace middleware;

TEST(ProxyTest, MiddlewareCreation) {
    ProxyOptions opts;
    opts.target_host = "localhost";
    opts.target_port = 8080;
    
    auto m = proxy(opts);
    EXPECT_NE(m, nullptr);
}

TEST(ProxyTest, LoadBalancerCreation) {
    LoadBalancerOptions opts;
    opts.nodes.push_back({"localhost", 8081});
    opts.nodes.push_back({"localhost", 8082});
    
    auto m = load_balancer(opts);
    EXPECT_NE(m, nullptr);
}

TEST(ProxyTest, LoadBalancerThrowsOnEmptyNodes) {
    LoadBalancerOptions opts;
    EXPECT_THROW(load_balancer(opts), std::runtime_error);
}
