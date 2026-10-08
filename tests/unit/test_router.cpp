#include <gtest/gtest.h>
#include <orbit/routing/Router.hpp>
#include <orbit/http/HttpParser.hpp>
#include <orbit/network/Proactor.hpp>
#include <orbit/concurrency/ThreadPool.hpp>

#include <orbit/network/PlatformSocket.hpp>

class DummyProactor : public orbit::network::Proactor {
public:
    void run_once(int) override {}
    void async_read(orbit::network::socket_t, void*, size_t, std::function<void(ssize_t)>) override {}
    void async_write(orbit::network::socket_t, const void*, size_t, std::function<void(ssize_t)>) override {}
    void async_wait_read(orbit::network::socket_t, std::function<void()>) override {}
    void async_wait_write(orbit::network::socket_t, std::function<void()>) override {}
    void async_sendfile(orbit::network::socket_t, int, off_t, size_t, std::function<void(ssize_t)>) override {}
    void async_accept(orbit::network::socket_t, std::function<void(orbit::network::socket_t, sockaddr_in)>) override {}
    void async_connect(orbit::network::socket_t, const sockaddr_in&, std::function<void(int)>) override {}
    void remove(orbit::network::socket_t) override {}
};

class MockResponseWriter : public orbit::http::ResponseWriter {
public:
    orbit::http::HttpResponse last_response;
    DummyProactor dummy_proactor;
    orbit::concurrency::ThreadPool dummy_thread_pool{1};
    std::vector<Interceptor> interceptors_;

    MockResponseWriter() = default;
    
    void add_interceptor(Interceptor interceptor) override {
        interceptors_.push_back(std::move(interceptor));
    }

    orbit::network::Proactor& proactor() override { return dummy_proactor; }
    orbit::concurrency::ThreadPool& thread_pool() override { return dummy_thread_pool; }

    void set_header(const std::string& key, const std::string& value) override {
        last_response.headers[key] = value;
    }

    void send_headers(orbit::http::HttpResponse& response) override {
        for (const auto& [k, v] : response.headers) {
            last_response.headers[k] = v;
        }
    }
    
    void send(orbit::http::HttpResponse&& response) override {
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
    orbit::routing::Router router;
    router.add_route(orbit::http::HttpMethod::GET, "/test", [](const orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
        orbit::http::HttpResponse res;
        res.status_code = orbit::http::HttpStatus::OK;
        writer->send(std::move(res));
    });

    orbit::http::HttpRequest req1;
    req1.method = orbit::http::HttpMethod::GET;
    req1.uri = "/test";
    
    auto writer1 = std::make_shared<MockResponseWriter>();
    router.route(req1, writer1);
    EXPECT_EQ(writer1->last_response.status_code, orbit::http::HttpStatus::OK);

    orbit::http::HttpRequest req2;
    req2.method = orbit::http::HttpMethod::GET;
    req2.uri = "/unknown";
    
    auto writer2 = std::make_shared<MockResponseWriter>();
    router.route(req2, writer2);
    EXPECT_EQ(writer2->last_response.status_code, orbit::http::HttpStatus::NotFound);
}

TEST(RouterTest, DynamicRouteParameters) {
    orbit::routing::Router router;
    router.add_route(orbit::http::HttpMethod::GET, "/users/:id", [](const orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
        orbit::http::HttpResponse res;
        res.status_code = orbit::http::HttpStatus::OK;
        // Verify params are extracted
        EXPECT_EQ(req.params.at("id"), "42");
        writer->send(std::move(res));
    });

    orbit::http::HttpRequest req1;
    req1.method = orbit::http::HttpMethod::GET;
    req1.uri = "/users/42";
    
    auto writer = std::make_shared<MockResponseWriter>();
    router.route(req1, writer);
    EXPECT_EQ(writer->last_response.status_code, orbit::http::HttpStatus::OK);
}

TEST(RouterTest, MiddlewareExecution) {
    orbit::routing::Router router;
    
    // Middleware that blocks requests
    router.use([](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
        if (req.headers["Authorization"] != "Bearer token") {
            orbit::http::HttpResponse res;
            res.status_code = orbit::http::HttpStatus::Forbidden;
            writer->send(std::move(res));
            return false; // Stop pipeline
        }
        return true; // Continue
    });

    router.add_route(orbit::http::HttpMethod::GET, "/protected", [](const orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter> writer) {
        orbit::http::HttpResponse res;
        res.status_code = orbit::http::HttpStatus::OK;
        writer->send(std::move(res));
    });

    // Request without auth
    orbit::http::HttpRequest req1;
    req1.method = orbit::http::HttpMethod::GET;
    req1.uri = "/protected";
    auto writer1 = std::make_shared<MockResponseWriter>();
    router.route(req1, writer1);
    EXPECT_EQ(writer1->last_response.status_code, orbit::http::HttpStatus::Forbidden);

    // Request with auth
    orbit::http::HttpRequest req2;
    req2.method = orbit::http::HttpMethod::GET;
    req2.uri = "/protected";
    req2.headers["Authorization"] = "Bearer token";
    auto writer2 = std::make_shared<MockResponseWriter>();
    router.route(req2, writer2);
    EXPECT_EQ(writer2->last_response.status_code, orbit::http::HttpStatus::OK);
}

TEST(RouterDecodingTest, DynamicParamsAreDecoded) {
    orbit::routing::Router router;
    std::string captured;
    router.get("/users/:name", [&](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter>) {
        captured = req.params["name"];
    });
    auto req = orbit::http::HttpParser::parse("GET /users/Jos%C3%A9%20M HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(req.has_value());
    router.route(*req, nullptr);
    EXPECT_EQ(captured, "Jos\xC3\xA9 M");
}

// --- Nested groups (issue #26) ---

namespace {
orbit::routing::Middleware tag(std::vector<std::string>& log, std::string name) {
    return [&log, name](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) {
        log.push_back(name);
        return true;
    };
}

orbit::http::HttpRequest get_request(const std::string& uri, orbit::http::HttpMethod method = orbit::http::HttpMethod::GET) {
    orbit::http::HttpRequest req;
    req.method = method;
    req.uri = uri;
    return req;
}
} // namespace

TEST(RouterGroupTest, NestedGroupsKeepRoutesPrefixesAndMiddleware) {
    orbit::routing::Router router;
    std::vector<std::string> log;
    router.group("/api", [&](orbit::routing::Router& api) {
        api.use(tag(log, "api"));
        api.get("/ping", [&](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) { log.push_back("ping"); });
        api.group("/v1", [&](orbit::routing::Router& v1) {
            v1.use(tag(log, "v1"));
            v1.get("/users", [&](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) { log.push_back("users"); });
            v1.group("/admin", [&](orbit::routing::Router& admin) {
                admin.use(tag(log, "admin"));
                admin.get("/stats/:id", [&](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter>) {
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
    orbit::routing::Router router;
    std::vector<std::string> log;
    router.group("/up", [&](orbit::routing::Router& up) {
        up.use(tag(log, "auth"));
        up.group("/files", [&](orbit::routing::Router& files) {
            files.add_stream_route(orbit::http::HttpMethod::POST, "/raw", [&](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) {
                log.push_back("stream");
            });
        });
    });
    EXPECT_TRUE(router.is_stream_route(orbit::http::HttpMethod::POST, "/up/files/raw"));
    auto req = get_request("/up/files/raw", orbit::http::HttpMethod::POST);
    router.route(req, nullptr);
    EXPECT_EQ(log, (std::vector<std::string>{"auth", "stream"}));
}
