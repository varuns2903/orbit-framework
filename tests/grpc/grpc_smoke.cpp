// Smoke test for the experimental gRPC wrapper (server::GrpcServer), run by
// .github/workflows/grpc.yml against the distribution's gRPC. It is not part
// of the main test binary, which is built without gRPC.

#include <orbit/server/GrpcServer.hpp>
#include <grpcpp/grpcpp.h>
#include "echo.grpc.pb.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (!ok) ++failures;
}

class EchoImpl final : public orbittest::Echo::Service {
    grpc::Status Say(grpc::ServerContext*, const orbittest::Msg* in, orbittest::Msg* out) override {
        out->set_text("echo: " + in->text());
        return grpc::Status::OK;
    }
};

// The reply text, or "" if the call failed.
std::string say(int port, const std::string& text) {
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
    auto stub = orbittest::Echo::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    orbittest::Msg req, res;
    req.set_text(text);
    return stub->Say(&ctx, req, &res).ok() ? res.text() : "";
}

bool start_throws(server::GrpcServer& s, const std::string& address) {
    try {
        s.start(address);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    EchoImpl service;

    server::GrpcServer s;
    s.add_service(&service);
    s.start("127.0.0.1:0");
    const int port = s.port();
    check(port > 0, "port 0 resolves to the port actually listened on");
    check(say(port, "hi") == "echo: hi", "a registered service answers");

    s.stop();
    check(s.port() == 0, "stop() clears the port");
    check(say(port, "hi").empty(), "nothing answers after stop()");

    s.start("127.0.0.1:0");
    check(say(s.port(), "again") == "echo: again", "the server can be started again after stop()");
    s.stop();

    server::GrpcServer bad;
    bad.add_service(&service);
    check(start_throws(bad, "256.256.256.256:1"), "an address that cannot be bound throws");
    check(bad.port() == 0, "a failed start leaves no port");

    std::cout << (failures ? "FAILED" : "PASSED") << "\n";
    return failures ? 1 : 0;
}
