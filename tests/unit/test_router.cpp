#include <gtest/gtest.h>
#include <orbit/routing/Router.hpp>
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
