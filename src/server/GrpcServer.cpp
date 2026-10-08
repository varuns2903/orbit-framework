#include <orbit/server/GrpcServer.hpp>
#include <stdexcept>

#ifdef ORBIT_ENABLE_GRPC
#include <grpcpp/grpcpp.h>

namespace orbit::server {

GrpcServer::GrpcServer() = default;

GrpcServer::~GrpcServer() {
    stop();
}

void GrpcServer::add_service(grpc::Service* service) {
    services_.push_back(service);
}

void GrpcServer::start(const std::string& address) {
    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &selected_port);

    for (auto* service : services_) {
        builder.RegisterService(service);
    }

    // BuildAndStart() returns null, and selected_port stays 0, when the
    // server cannot start (e.g. an address that cannot be bound). Ignoring
    // that made a failed start look like a running server.
    server_ = builder.BuildAndStart();
    if (!server_ || selected_port == 0) {
        server_.reset();
        throw std::runtime_error("gRPC server could not start on " + address);
    }
    port_ = selected_port;
}

void GrpcServer::stop() {
    if (server_) {
        server_->Shutdown();
        server_.reset();
    }
    port_ = 0;
}

} // namespace server
#else
// Built without gRPC: fail loudly instead of pretending to serve.
namespace orbit::server {

namespace {
[[noreturn]] void no_grpc() {
    throw std::runtime_error("Orbit was built without gRPC support; configure with -DORBIT_ENABLE_GRPC=ON");
}
} // namespace

GrpcServer::GrpcServer() {}
GrpcServer::~GrpcServer() {}
void GrpcServer::add_service(grpc::Service*) { no_grpc(); }
void GrpcServer::start(const std::string&) { no_grpc(); }
void GrpcServer::stop() {}

} // namespace server
#endif
