#include <gtest/gtest.h>
#include <orbit/testing/Client.hpp>
#include <orbit/server/App.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/concurrency/ThreadPool.hpp>
#include <orbit/middleware/SessionManager.hpp>
#include <orbit/middleware/SecurityHeaders.hpp>

#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// orbit::testing::Client (#203): an App's routes called in-process, with no
// listen() and no sockets.

using namespace orbit::http;
using orbit::concurrency::Task;
using orbit::testing::Client;

namespace {

// Resumes the coroutine on the writer's thread pool, as a database callback
// would: the response arrives from another thread, later.
struct ResumeOnPool {
    orbit::concurrency::ThreadPool& pool;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        pool.enqueue([h] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            h.resume();
        });
    }
    void await_resume() const noexcept {}
};

void reply(const std::shared_ptr<ResponseWriter>& w, HttpStatus status, std::string body) {
    HttpResponse res;
    res.status(status).send(std::move(body));
    w->send(std::move(res));
}

} // namespace

class TestingClientTest : public ::testing::Test {
protected:
    orbit::server::App app{orbit::config::ServerConfig{}};

    void SetUp() override {
        app.get("/hello", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            reply(w, HttpStatus::OK, "hello " + req.query["name"]);
        });
        app.post("/items", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            auto body = req.json();
            HttpResponse res;
            res.status(HttpStatus::Created).json(nlohmann::json{{"title", body.value("title", "")}, {"id", 1}});
            w->send(std::move(res));
        });
        app.get("/items/:id", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
            reply(w, HttpStatus::OK, "item " + req.params["id"]);
        });
        app.get("/boom", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::runtime_error("secret"); });
        app.get("/slow", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) -> Task {
            co_await ResumeOnPool{w->thread_pool()};
            reply(w, HttpStatus::OK, "late " + std::string(req.headers.at("X-Who")));
        });
        app.get("/events", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            res.headers["Content-Type"] = "text/event-stream";
            w->send_headers(res);
            w->send_sse_event("one");
            w->send_sse_event("two", "update", "2");
            // Never ends: an open stream.
        });
        app.get("/chunked", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            HttpResponse res;
            w->send_headers(res);
            w->write_chunk("a");
            w->write_chunk("b");
            w->end();
        });
    }
};

TEST_F(TestingClientTest, RoutesQueryAndParams) {
    Client client(app);
    auto res = client.get("/hello?name=orbit").send();
    EXPECT_TRUE(res.complete);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body, "hello orbit");
    EXPECT_EQ(client.get("/items/42").send().body, "item 42");
}

TEST_F(TestingClientTest, JsonBodiesBothWays) {
    Client client(app);
    auto res = client.post("/items").json({{"title", "write tests"}}).send();
    EXPECT_EQ(res.status, 201);
    EXPECT_EQ(res.json()["title"], "write tests");
    EXPECT_NE(res.header("content-type").value_or("").find("application/json"), std::string::npos);
}

TEST_F(TestingClientTest, NotFoundMethodNotAllowedAndErrors) {
    Client client(app);
    EXPECT_EQ(client.get("/nowhere").send().status, 404);
    auto wrong = client.del("/items/1").send();
    EXPECT_EQ(wrong.status, 405);
    EXPECT_EQ(wrong.header("Allow"), "GET, HEAD");
    auto boom = client.get("/boom").send();
    EXPECT_EQ(boom.status, 500);
    EXPECT_EQ(boom.body.find("secret"), std::string::npos);

    app.on_error([](const std::exception& e, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        reply(w, HttpStatus::BadGateway, std::string("handled: ") + e.what());
    });
    EXPECT_EQ(client.get("/boom").send().body, "handled: secret");
}

TEST_F(TestingClientTest, MiddlewareRunsAsOnTheNetwork) {
    app.use("/admin", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        if (req.headers.find("Authorization") != req.headers.end()) return true;
        reply(w, HttpStatus::Unauthorized, "no");
        return false;
    });
    app.get("/admin/panel", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { reply(w, HttpStatus::OK, "panel"); });
    orbit::middleware::SecurityHeadersOptions headers;
    headers.hsts = false;
    app.use(orbit::middleware::security_headers(headers));

    Client client(app);
    EXPECT_EQ(client.get("/admin/panel").send().status, 401);
    auto ok = client.get("/admin/panel").header("Authorization", "Bearer x").send();
    EXPECT_EQ(ok.status, 200);
    EXPECT_EQ(ok.header("X-Content-Type-Options"), "nosniff") << "headers a middleware sets are applied";
}

TEST_F(TestingClientTest, CoroutineHandlersAreAwaited) {
    Client client(app);
    auto res = client.get("/slow").header("X-Who", "coroutine").send();
    EXPECT_TRUE(res.complete);
    EXPECT_EQ(res.body, "late coroutine");
}

TEST_F(TestingClientTest, StreamsAreCollected) {
    Client client(app);
    auto chunked = client.get("/chunked").send();
    EXPECT_TRUE(chunked.complete);
    EXPECT_EQ(chunked.chunks, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(chunked.body, "ab");

    // An endless SSE stream: send() returns after its timeout with what came.
    auto sse = client.get("/events").send(std::chrono::milliseconds(100));
    EXPECT_FALSE(sse.complete);
    ASSERT_EQ(sse.chunks.size(), 2u);
    EXPECT_EQ(sse.chunks[0], "data: one\n\n");
    EXPECT_EQ(sse.chunks[1], "event: update\nid: 2\ndata: two\n\n");
}

// The client leaves once send() returns: on_close fires for a held writer.
TEST_F(TestingClientTest, HeldWritersSeeTheClientLeave) {
    std::shared_ptr<ResponseWriter> held;
    app.get("/hold", [&held](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        held = w;
        HttpResponse res;
        w->send_headers(res);
    });
    Client client(app);
    client.get("/hold").send(std::chrono::milliseconds(20));
    ASSERT_TRUE(held);
    EXPECT_FALSE(held->is_open());
}

TEST_F(TestingClientTest, CookieJarCarriesTheSession) {
    app.use(orbit::middleware::session(std::make_shared<orbit::middleware::MemorySessionStore>()));
    app.post("/login", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        req.session->set("user", "ada");
        reply(w, HttpStatus::OK, "in");
    });
    app.get("/me", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        reply(w, HttpStatus::OK, req.session ? req.session->get("user").value_or("anonymous") : "no session");
    });

    Client client(app);
    EXPECT_EQ(client.get("/me").send().body, "anonymous");
    auto login = client.post("/login").send();
    EXPECT_EQ(login.status, 200);
    EXPECT_FALSE(client.cookies().empty()) << "the session cookie went into the jar";
    EXPECT_EQ(client.get("/me").send().body, "ada");

    Client stranger(app);
    EXPECT_EQ(stranger.get("/me").send().body, "anonymous");
}

TEST_F(TestingClientTest, HeadDropsTheBodyAndBadTargetsAre400) {
    Client client(app);
    auto head = client.head("/hello").send();
    EXPECT_EQ(head.status, 200);
    EXPECT_TRUE(head.body.empty());
    EXPECT_EQ(client.get("/bad%zzpath").send().status, 400);
}
