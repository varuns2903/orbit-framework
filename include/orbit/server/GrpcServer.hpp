#pragma once
#include <memory>
#include <string>
#include <vector>

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
#ifdef ORBIT_ENABLE_GRPC
    std::unique_ptr<grpc::Server> server_;
    std::vector<grpc::Service*> services_;
#endif
};

} // namespace server
