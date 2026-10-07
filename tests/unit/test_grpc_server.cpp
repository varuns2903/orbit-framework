#include <gtest/gtest.h>
#include <orbit/server/GrpcServer.hpp>
#include <stdexcept>
#include <string>

// The gRPC wrapper itself is exercised by tests/grpc (see
// .github/workflows/grpc.yml). This binary is built without gRPC, where the
// wrapper must refuse to work rather than pretend to serve.

#ifndef ORBIT_ENABLE_GRPC
TEST(GrpcServerTest, WithoutGrpcSupportStartAndAddServiceThrow) {
    server::GrpcServer s;
    try {
        s.start("127.0.0.1:0");
        FAIL() << "start() succeeded in a build without gRPC";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("ORBIT_ENABLE_GRPC=ON"), std::string::npos) << e.what();
    }
    EXPECT_THROW(s.add_service(nullptr), std::runtime_error);
    EXPECT_EQ(s.port(), 0);
    s.stop(); // harmless
}
#endif
