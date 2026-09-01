#pragma once
#include <memory>
#include <string>

// Forward declare grpc::Server to avoid dragging gRPC headers into Orbit headers
namespace grpc {
    class Server;
    class Service;
}

namespace server {

class GrpcServer {
public:
    GrpcServer();
    ~GrpcServer();

    void add_service(grpc::Service* service);
    void start(const std::string& address);
    void stop();

private:
    std::unique_ptr<grpc::Server> server_;
    std::vector<grpc::Service*> services_;
};

} // namespace server
