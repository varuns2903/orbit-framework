#include <orbit/server/GrpcServer.hpp>

#ifdef ORBIT_ENABLE_GRPC
#include <grpcpp/grpcpp.h>

namespace server {

GrpcServer::GrpcServer() = default;

GrpcServer::~GrpcServer() {
    stop();
}

void GrpcServer::add_service(grpc::Service* service) {
    services_.push_back(service);
}

void GrpcServer::start(const std::string& address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    
    for (auto* service : services_) {
        builder.RegisterService(service);
    }
    
    server_ = builder.BuildAndStart();
}

void GrpcServer::stop() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
    }
}

} // namespace server
#else
// Dummy implementation if gRPC is not enabled
namespace server {
    GrpcServer::GrpcServer() {}
    GrpcServer::~GrpcServer() {}
    void GrpcServer::add_service(grpc::Service*) {}
    void GrpcServer::start(const std::string&) {}
    void GrpcServer::stop() {}
}
#endif
