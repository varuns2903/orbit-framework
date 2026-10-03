#include <gtest/gtest.h>
#include <orbit/routing/Router.hpp>
#include <orbit/http/HttpParser.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <orbit/network/PlatformSocket.hpp>

class DummyProactor : public network::Proactor {
public:
    void run_once(int) override {}
    void async_read(network::socket_t, void*, size_t, std::function<void(ssize_t)>) override {}
    void async_write(network::socket_t, const void*, size_t, std::function<void(ssize_t)>) override {}
    void async_wait_read(network::socket_t, std::function<void()>) override {}
    void async_wait_write(network::socket_t, std::function<void()>) override {}
    void async_sendfile(network::socket_t, int, off_t, size_t, std::function<void(ssize_t)>) override {}
    void async_accept(network::socket_t, std::function<void(network::socket_t, sockaddr_in)>) override {}
    void async_connect(network::socket_t, const sockaddr_in&, std::function<void(int)>) override {}
    void remove(network::socket_t) override {}
};

class MockResponseWriter : public http::ResponseWriter {
public:
    http::HttpResponse last_response;
    DummyProactor dummy_proactor;
    concurrency::ThreadPool dummy_thread_pool{1};
    std::vector<Interceptor> interceptors_;

    MockResponseWriter() = default;
    
    void add_interceptor(Interceptor interceptor) override {
        interceptors_.push_back(std::move(interceptor));
    }

    network::Proactor& proactor() override { return dummy_proactor; }
    concurrency::ThreadPool& thread_pool() override { return dummy_thread_pool; }

    void set_header(const std::string& key, const std::string& value) override {
        last_response.headers[key] = value;
    }

    void send_headers(http::HttpResponse& response) override {
        for (const auto& [k, v] : response.headers) {
            last_response.headers[k] = v;
        }
    }
    
    void send(http::HttpResponse&& response) override {
        for (const auto& [k, v] : last_response.headers) {
            if (response.headers.find(k) == response.headers.end()) {
                response.headers[k] = v;
            }
        }
        last_response = std::move(response);
    }
    
    void write_chunk(std::string_view chunk) override {
        last_response.body += chunk;
    }
    
    void end() override {}
    
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

TEST(RouterTest, RouteMatchAndNotFound) {
    routing::Router router;
    router.add_route(http::HttpMethod::GET, "/test", [](const http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
        http::HttpResponse res;
        res.status_code = http::HttpStatus::OK;
        writer->send(std::move(res));
    });

    http::HttpRequest req1;
    req1.method = http::HttpMethod::GET;
    req1.uri = "/test";
    
    auto writer1 = std::make_shared<MockResponseWriter>();
    router.route(req1, writer1);
    EXPECT_EQ(writer1->last_response.status_code, http::HttpStatus::OK);

    http::HttpRequest req2;
    req2.method = http::HttpMethod::GET;
    req2.uri = "/unknown";
    
    auto writer2 = std::make_shared<MockResponseWriter>();
    router.route(req2, writer2);
    EXPECT_EQ(writer2->last_response.status_code, http::HttpStatus::NotFound);
}

TEST(RouterTest, DynamicRouteParameters) {
    routing::Router router;
    router.add_route(http::HttpMethod::GET, "/users/:id", [](const http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
        http::HttpResponse res;
        res.status_code = http::HttpStatus::OK;
        // Verify params are extracted
        EXPECT_EQ(req.params.at("id"), "42");
        writer->send(std::move(res));
    });

    http::HttpRequest req1;
    req1.method = http::HttpMethod::GET;
    req1.uri = "/users/42";
    
    auto writer = std::make_shared<MockResponseWriter>();
    router.route(req1, writer);
    EXPECT_EQ(writer->last_response.status_code, http::HttpStatus::OK);
}

TEST(RouterTest, MiddlewareExecution) {
    routing::Router router;
    
    // Middleware that blocks requests
    router.use([](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
        if (req.headers["Authorization"] != "Bearer token") {
            http::HttpResponse res;
            res.status_code = http::HttpStatus::Forbidden;
            writer->send(std::move(res));
            return false; // Stop pipeline
        }
        return true; // Continue
    });

    router.add_route(http::HttpMethod::GET, "/protected", [](const http::HttpRequest&, std::shared_ptr<http::ResponseWriter> writer) {
        http::HttpResponse res;
        res.status_code = http::HttpStatus::OK;
        writer->send(std::move(res));
    });

    // Request without auth
    http::HttpRequest req1;
    req1.method = http::HttpMethod::GET;
    req1.uri = "/protected";
    auto writer1 = std::make_shared<MockResponseWriter>();
    router.route(req1, writer1);
    EXPECT_EQ(writer1->last_response.status_code, http::HttpStatus::Forbidden);

    // Request with auth
    http::HttpRequest req2;
    req2.method = http::HttpMethod::GET;
    req2.uri = "/protected";
    req2.headers["Authorization"] = "Bearer token";
    auto writer2 = std::make_shared<MockResponseWriter>();
    router.route(req2, writer2);
    EXPECT_EQ(writer2->last_response.status_code, http::HttpStatus::OK);
}

TEST(RouterDecodingTest, DynamicParamsAreDecoded) {
    routing::Router router;
    std::string captured;
    router.get("/users/:name", [&](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter>) {
        captured = req.params["name"];
    });
    auto req = http::HttpParser::parse("GET /users/Jos%C3%A9%20M HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    router.route(*req, nullptr);
    EXPECT_EQ(captured, "Jos\xC3\xA9 M");
}

// --- Nested groups (issue #26) ---

namespace {
routing::Middleware tag(std::vector<std::string>& log, std::string name) {
    return [&log, name](http::HttpRequest&, std::shared_ptr<http::ResponseWriter>) {
        log.push_back(name);
        return true;
    };
}

http::HttpRequest get_request(const std::string& uri, http::HttpMethod method = http::HttpMethod::GET) {
    http::HttpRequest req;
    req.method = method;
    req.uri = uri;
    return req;
}
} // namespace

TEST(RouterGroupTest, NestedGroupsKeepRoutesPrefixesAndMiddleware) {
    routing::Router router;
    std::vector<std::string> log;
    router.group("/api", [&](routing::Router& api) {
        api.use(tag(log, "api"));
        api.get("/ping", [&](http::HttpRequest&, std::shared_ptr<http::ResponseWriter>) { log.push_back("ping"); });
        api.group("/v1", [&](routing::Router& v1) {
            v1.use(tag(log, "v1"));
            v1.get("/users", [&](http::HttpRequest&, std::shared_ptr<http::ResponseWriter>) { log.push_back("users"); });
            v1.group("/admin", [&](routing::Router& admin) {
                admin.use(tag(log, "admin"));
                admin.get("/stats/:id", [&](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter>) {
                    log.push_back("stats " + req.params["id"]);
                });
            });
        });
    });

    auto r1 = get_request("/api/ping");
    router.route(r1, nullptr);
    EXPECT_EQ(log, (std::vector<std::string>{"api", "ping"}));

    log.clear();
    auto r2 = get_request("/api/v1/users");
    router.route(r2, nullptr);
    EXPECT_EQ(log, (std::vector<std::string>{"api", "v1", "users"}));

    log.clear();
    auto r3 = get_request("/api/v1/admin/stats/42");
    router.route(r3, nullptr);
    EXPECT_EQ(log, (std::vector<std::string>{"api", "v1", "admin", "stats 42"}));
}

TEST(RouterGroupTest, StreamRoutesInGroupsRunGroupMiddleware) {
    routing::Router router;
    std::vector<std::string> log;
    router.group("/up", [&](routing::Router& up) {
        up.use(tag(log, "auth"));
        up.group("/files", [&](routing::Router& files) {
            files.add_stream_route(http::HttpMethod::POST, "/raw", [&](http::HttpRequest&, std::shared_ptr<http::ResponseWriter>) {
                log.push_back("stream");
            });
        });
    });
    EXPECT_TRUE(router.is_stream_route(http::HttpMethod::POST, "/up/files/raw"));
    auto req = get_request("/up/files/raw", http::HttpMethod::POST);
    router.route(req, nullptr);
    EXPECT_EQ(log, (std::vector<std::string>{"auth", "stream"}));
}
