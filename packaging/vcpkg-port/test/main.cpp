// Starts an App, sends it one request over a socket, and checks the reply.
#include <orbit/server/App.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

int main() {
    orbit::config::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 8199;
    orbit::server::App app(cfg);
    app.get("/", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> w) {
        orbit::http::HttpResponse res;
        res.set_body("hello from vcpkg", "text/plain");
        w->send(std::move(res));
    });
    std::thread server([&] { app.listen(); });

    std::string reply;
    for (int attempt = 0; attempt < 50 && reply.empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8199);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            const char req[] = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
            ::send(fd, req, sizeof(req) - 1, 0);
            char buf[1024];
            ssize_t n;
            while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) reply.append(buf, static_cast<size_t>(n));
        }
        ::close(fd);
    }

    app.stop();
    server.join();
    bool ok = reply.find("200") != std::string::npos && reply.find("hello from vcpkg") != std::string::npos;
    std::printf("%s\n", ok ? "consumer OK" : ("unexpected reply:\n" + reply).c_str());
    return ok ? 0 : 1;
}
