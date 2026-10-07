#pragma once
#include <memory>
#include <string>
#include <vector>

namespace grpc {
    class Server;
    class Service;
}

namespace server {

/**
 * @brief A thin wrapper that runs gRPC services next to an Orbit App.
 *
 * @warning Experimental. It is a minimal pass-through to grpc::ServerBuilder
 * (insecure credentials only, no TLS, no options) and is not covered by the
 * main CI matrix; see issue #22. Build with -DORBIT_ENABLE_GRPC=ON. Without
 * it, add_service() and start() throw, rather than pretending to serve.
 *
 * gRPC enables SO_REUSEPORT on Linux, so two servers (or two processes, as
 * during App::hot_reload()) can listen on the same address and share its
 * connections.
 */
class GrpcServer {
public:
    GrpcServer();
    ~GrpcServer();

    /// Registers a service; it must outlive the server. Call before start().
    void add_service(grpc::Service* service);

    /// Starts serving on `address` ("host:port"; port 0 picks a free one).
    /// @throws std::runtime_error if the server could not be started.
    void start(const std::string& address);

    /// The port listened on once started, e.g. the one chosen for port 0.
    int port() const { return port_; }

    void stop();

private:
#ifdef ORBIT_ENABLE_GRPC
    std::unique_ptr<grpc::Server> server_;
    std::vector<grpc::Service*> services_;
#endif
    int port_ = 0;
};

} // namespace server
