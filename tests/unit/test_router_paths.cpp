#include <gtest/gtest.h>
#include <orbit/routing/Router.hpp>
#include <orbit/openapi/OpenApi.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace orbit::routing;
using namespace orbit::http;

namespace {

class RouterPathsMockWriter : public ResponseWriter {
public:
    std::vector<HttpResponse> sent;

    const HttpResponse& last() const { return sent.back(); }

    void send(HttpResponse&& response) override {
        mark_responded();
        sent.push_back(std::move(response));
    }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("Not implemented"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("Not implemented"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

// Routes one request and returns the writer that saw the result.
std::shared_ptr<RouterPathsMockWriter> dispatch(const Router& router, HttpMethod method, const std::string& uri) {
    HttpRequest req;
    req.method = method;
    req.uri = uri;
    auto writer = std::make_shared<RouterPathsMockWriter>();
    router.route(req, writer);
    return writer;
}

// A handler that replies with a fixed body.
RouteHandler reply(std::string body) {
    return [body](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send(body);
        w->send(std::move(res));
    };
}

std::string body_of(const std::shared_ptr<RouterPathsMockWriter>& w) {
    return w->sent.empty() ? "<nothing sent>" : w->last().body;
}

} // namespace

// --- Method dispatch ---

TEST(RouterPathsTest, EveryVerbHelperRegistersItsMethod) {
    Router r;
    r.get("/x", reply("get"));
    r.post("/x", reply("post"));
    r.put("/x", reply("put"));
    r.patch("/x", reply("patch"));
    r.del("/x", reply("delete"));
    r.options("/x", reply("options"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/x")), "get");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::POST, "/x")), "post");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::PUT, "/x")), "put");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::PATCH, "/x")), "patch");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::DELETE, "/x")), "delete");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::OPTIONS, "/x")), "options");
}

TEST(RouterPathsTest, VerbHelpersWithMiddlewareRunItFirst) {
    Router r;
    auto tag = [](const char* t) {
        return Middleware([t](HttpRequest& req, std::shared_ptr<ResponseWriter>) {
            req.params["mw"] = t;
            return true;
        });
    };
    auto echo_mw = [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send(req.params["mw"]);
        w->send(std::move(res));
    };
    r.get("/m", {tag("get")}, echo_mw);
    r.post("/m", {tag("post")}, echo_mw);
    r.put("/m", {tag("put")}, echo_mw);
    r.patch("/m", {tag("patch")}, echo_mw);
    r.del("/m", {tag("delete")}, echo_mw);
    r.options("/m", {tag("options")}, echo_mw);
    for (auto [m, expect] : {std::pair{HttpMethod::GET, "get"}, {HttpMethod::POST, "post"},
                             {HttpMethod::PUT, "put"}, {HttpMethod::PATCH, "patch"},
                             {HttpMethod::DELETE, "delete"}, {HttpMethod::OPTIONS, "options"}}) {
        EXPECT_EQ(body_of(dispatch(r, m, "/m")), expect);
    }
}

TEST(RouterPathsTest, RouteMiddlewareCanStopTheHandler) {
    Router r;
    bool handler_ran = false;
    Middleware deny = [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::Forbidden).send("no");
        w->send(std::move(res));
        return false;
    };
    r.get("/guarded", {deny}, [&](HttpRequest&, std::shared_ptr<ResponseWriter>) { handler_ran = true; });
    auto w = dispatch(r, HttpMethod::GET, "/guarded");
    EXPECT_FALSE(handler_ran);
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::Forbidden);
}

TEST(RouterPathsTest, HeadFallsBackToGet) {
    Router r;
    r.get("/page", reply("page"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::HEAD, "/page")), "page");
}

TEST(RouterPathsTest, ExactRouteWinsOverDynamic) {
    Router r;
    r.get("/users/:id", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send("user " + req.params["id"]);
        w->send(std::move(res));
    });
    r.get("/users/me", reply("me"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/users/me")), "me");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/users/42")), "user 42");
}

TEST(RouterPathsTest, DynamicRouteNeedsTheSameNumberOfSegments) {
    Router r;
    r.get("/a/:x", reply("one"));
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/a")->last().status_code, HttpStatus::NotFound);
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/a/1/2")->last().status_code, HttpStatus::NotFound);
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/a/1")), "one");
}

// --- 404 and 405 ---

TEST(RouterPathsTest, UnknownPathIs404) {
    Router r;
    r.get("/known", reply("k"));
    auto w = dispatch(r, HttpMethod::GET, "/unknown");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::NotFound);
    EXPECT_EQ(w->last().headers.count("Allow"), 0u);
}

TEST(RouterPathsTest, WrongMethodIs405ListingTheOthersInOrder) {
    Router r;
    r.del("/thing", reply("d"));
    r.post("/thing", reply("p"));
    r.options("/thing", reply("o"));
    r.get("/thing", reply("g"));
    auto w = dispatch(r, HttpMethod::PUT, "/thing");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::MethodNotAllowed);
    // HEAD is implied by GET; the order is fixed, not registration order.
    EXPECT_EQ(w->last().headers.at("Allow"), "GET, HEAD, POST, DELETE, OPTIONS");
}

TEST(RouterPathsTest, AllowWithoutGetHasNoHead) {
    Router r;
    r.post("/submit", reply("p"));
    auto w = dispatch(r, HttpMethod::GET, "/submit");
    EXPECT_EQ(w->last().status_code, HttpStatus::MethodNotAllowed);
    EXPECT_EQ(w->last().headers.at("Allow"), "POST");
}

TEST(RouterPathsTest, DynamicRoutesCountFor405) {
    Router r;
    r.patch("/items/:id", reply("patched"));
    auto w = dispatch(r, HttpMethod::GET, "/items/9");
    EXPECT_EQ(w->last().status_code, HttpStatus::MethodNotAllowed);
    EXPECT_EQ(w->last().headers.at("Allow"), "PATCH");
}

// --- Errors ---

TEST(RouterPathsTest, HandlerExceptionIs500WithoutItsMessage) {
    Router r;
    r.get("/boom", [](HttpRequest&, std::shared_ptr<ResponseWriter>) {
        throw std::runtime_error("SELECT password FROM users");
    });
    auto w = dispatch(r, HttpMethod::GET, "/boom");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
    EXPECT_EQ(w->last().body.find("password"), std::string::npos) << w->last().body;
}

TEST(RouterPathsTest, NonStandardExceptionIs500) {
    Router r;
    r.get("/int", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw 42; });
    auto w = dispatch(r, HttpMethod::GET, "/int");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
}

TEST(RouterPathsTest, ErrorHandlerReceivesTheException) {
    Router r;
    r.on_error([](const std::exception& e, HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::BadRequest).send(std::string("handled: ") + e.what() + " at " + req.uri);
        w->send(std::move(res));
    });
    r.get("/bad", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::invalid_argument("nope"); });
    auto w = dispatch(r, HttpMethod::GET, "/bad");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::BadRequest);
    EXPECT_EQ(w->last().body, "handled: nope at /bad");
}

// A handler that answers 500 with a fixed body.
ErrorHandler answer_with(std::string body) {
    return [body](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::ServiceUnavailable).send(body);
        w->send(std::move(res));
    };
}

RouteHandler throws() {
    return [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::runtime_error("boom"); };
}

// on_error in a group covers that group's routes only (#218); it used to
// replace the app's handler for every route.
TEST(RouterPathsTest, GroupErrorHandlerCoversOnlyItsGroup) {
    Router r;
    r.on_error(answer_with("app"));
    r.group("/api", [](Router& api) {
        api.on_error(answer_with("api"));
        api.get("/x", throws());
        api.get("/items/:id", throws());
    });
    r.group("/admin", [](Router& admin) {
        admin.on_error(answer_with("admin"));
        admin.get("/x", throws());
    });
    r.group("/plain", [](Router& plain) { plain.get("/x", throws()); });
    r.get("/top", throws());

    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/x")), "api");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/items/3")), "api");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/admin/x")), "admin");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/plain/x")), "app");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/top")), "app");
}

// The innermost handler wins; a nested group without one uses its parent's.
// Registration order does not matter: on_error after the routes still counts.
TEST(RouterPathsTest, NestedGroupsFallBackToTheEnclosingHandler) {
    Router r;
    r.group("/api", [](Router& api) {
        api.get("/x", throws());
        api.group("/v1", [](Router& v1) { v1.get("/x", throws()); });
        api.group("/v2", [](Router& v2) {
            v2.get("/x", throws());
            v2.on_error(answer_with("v2"));
        });
        api.on_error(answer_with("api"));
    });
    r.on_error(answer_with("app"));
    r.get("/top", throws());

    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/x")), "api");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/v1/x")), "api");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/v2/x")), "v2");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/top")), "app");
}

// Group middleware runs as part of the route, so its exceptions go to the
// group's handler; app-level middleware runs before matching and uses the app's.
TEST(RouterPathsTest, MiddlewareExceptionsUseTheirOwnScope) {
    Router r;
    r.on_error(answer_with("app"));
    r.group("/api", [](Router& api) {
        api.on_error(answer_with("api"));
        api.use([](HttpRequest&, std::shared_ptr<ResponseWriter>) -> bool { throw std::runtime_error("group mw"); });
        api.get("/x", reply("unreached"));
    });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/x")), "api");

    Router g;
    g.on_error(answer_with("app"));
    g.use([](HttpRequest&, std::shared_ptr<ResponseWriter>) -> bool { throw std::runtime_error("app mw"); });
    g.group("/api", [](Router& api) {
        api.on_error(answer_with("api"));
        api.get("/x", reply("unreached"));
    });
    EXPECT_EQ(body_of(dispatch(g, HttpMethod::GET, "/api/x")), "app");
}

// An exception reported later (a coroutine handler failing after a
// co_await) reaches the same group handler as a synchronous one.
TEST(RouterPathsTest, AsyncExceptionUsesTheRoutesGroupHandler) {
    Router r;
    r.on_error(answer_with("app"));
    std::shared_ptr<ResponseWriter> kept;
    r.group("/api", [&kept](Router& api) {
        api.on_error(answer_with("api"));
        api.get("/later", [&kept](HttpRequest&, std::shared_ptr<ResponseWriter> w) { kept = w; });
    });

    HttpRequest req;
    req.method = HttpMethod::GET;
    req.uri = "/api/later";
    auto writer = std::make_shared<RouterPathsMockWriter>();
    r.route(req, writer);
    ASSERT_TRUE(writer->sent.empty());
    ResponseWriter::report_async_exception(kept, writer->request_generation(),
                                           std::make_exception_ptr(std::runtime_error("late")));
    EXPECT_EQ(body_of(writer), "api");
}

// The error handler only sees std::exception; anything else is a plain 500.
TEST(RouterPathsTest, NonStandardExceptionSkipsTheErrorHandler) {
    Router r;
    bool called = false;
    r.on_error([&](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter>) { called = true; });
    r.get("/int", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw 7; });
    auto w = dispatch(r, HttpMethod::GET, "/int");
    EXPECT_FALSE(called);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
}

TEST(RouterPathsTest, GlobalMiddlewareExceptionReachesTheErrorHandler) {
    Router r;
    r.use([](HttpRequest&, std::shared_ptr<ResponseWriter>) -> bool { throw std::runtime_error("mw"); });
    r.on_error([](const std::exception& e, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::BadGateway).send(e.what());
        w->send(std::move(res));
    });
    r.get("/any", reply("unreached"));
    auto w = dispatch(r, HttpMethod::GET, "/any");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::BadGateway);
    EXPECT_EQ(w->last().body, "mw");
}

TEST(RouterPathsTest, GlobalMiddlewareCanAnswerBeforeRouting) {
    Router r;
    r.use([](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::TooManyRequests).send("slow down");
        w->send(std::move(res));
        return false;
    });
    r.get("/x", reply("unreached"));
    // Even an unknown path: the middleware answers, not the 404.
    auto w = dispatch(r, HttpMethod::GET, "/nowhere");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::TooManyRequests);
}

// An error handler that throws still gets the client an answer.
TEST(RouterPathsTest, ThrowingErrorHandlerStillAnswers500) {
    Router r;
    r.on_error([](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter>) {
        throw std::runtime_error("the error handler failed too");
    });
    r.get("/x", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::runtime_error("first"); });
    auto w = dispatch(r, HttpMethod::GET, "/x");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
    EXPECT_EQ(w->last().body.find("failed too"), std::string::npos);
}

TEST(RouterPathsTest, ErrorHandlerThrowingANonStandardValueStillAnswers500) {
    Router r;
    r.on_error([](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter>) { throw 5; });
    r.get("/x", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::runtime_error("first"); });
    auto w = dispatch(r, HttpMethod::GET, "/x");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
}

// If the handler already sent a response, no second one follows.
TEST(RouterPathsTest, ErrorHandlerThatRespondedThenThrewSendsNothingMore) {
    Router r;
    r.on_error([](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::Conflict).send("already answered");
        w->send(std::move(res));
        throw std::runtime_error("then failed");
    });
    r.get("/x", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { throw std::runtime_error("first"); });
    auto w = dispatch(r, HttpMethod::GET, "/x");
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::Conflict);
}

// --- WebSocket routes ---

TEST(RouterPathsTest, WebSocketRouteLookup) {
    Router r;
    r.ws("/chat", [](orbit::http::websocket::WebSocketConnection&) {});
    EXPECT_TRUE(r.has_ws_route("/chat"));
    EXPECT_NE(r.get_ws_route("/chat"), nullptr);
    EXPECT_FALSE(r.has_ws_route("/other"));
    EXPECT_EQ(r.get_ws_route("/other"), nullptr);
}

TEST(RouterPathsTest, WebSocketMiddlewaresRunGlobalThenRoute) {
    Router r;
    std::string order;
    r.use([&](HttpRequest&, std::shared_ptr<ResponseWriter>) { order += "global,"; return true; });
    r.ws("/ws", {[&](HttpRequest&, std::shared_ptr<ResponseWriter>) { order += "route"; return true; }},
         [](orbit::http::websocket::WebSocketConnection&) {});
    HttpRequest req;
    auto w = std::make_shared<RouterPathsMockWriter>();
    EXPECT_TRUE(r.run_ws_middlewares("/ws", req, w));
    EXPECT_EQ(order, "global,route");
    EXPECT_FALSE(r.run_ws_middlewares("/missing", req, w));
}

TEST(RouterPathsTest, WebSocketMiddlewareCanRefuse) {
    Router r;
    r.ws("/ws", {[](HttpRequest&, std::shared_ptr<ResponseWriter>) { return false; }},
         [](orbit::http::websocket::WebSocketConnection&) {});
    HttpRequest req;
    auto w = std::make_shared<RouterPathsMockWriter>();
    EXPECT_FALSE(r.run_ws_middlewares("/ws", req, w));
}

TEST(RouterPathsTest, WebSocketMiddlewareExceptionIs500) {
    Router r;
    r.ws("/ws", {[](HttpRequest&, std::shared_ptr<ResponseWriter>) -> bool { throw std::runtime_error("secret"); }},
         [](orbit::http::websocket::WebSocketConnection&) {});
    HttpRequest req;
    auto w = std::make_shared<RouterPathsMockWriter>();
    EXPECT_FALSE(r.run_ws_middlewares("/ws", req, w));
    ASSERT_EQ(w->sent.size(), 1u);
    EXPECT_EQ(w->last().status_code, HttpStatus::InternalServerError);
    EXPECT_EQ(w->last().body.find("secret"), std::string::npos);
}

TEST(RouterPathsTest, WebSocketRoutesInGroupsGetThePrefix) {
    Router r;
    r.group("/live", [](Router& g) { g.ws("/feed", [](orbit::http::websocket::WebSocketConnection&) {}); });
    EXPECT_TRUE(r.has_ws_route("/live/feed"));
    EXPECT_FALSE(r.has_ws_route("/feed"));
}

// --- Stream routes ---

TEST(RouterPathsTest, StreamRoutesAreMarkedPerMethod) {
    Router r;
    r.add_stream_route(HttpMethod::POST, "/upload", reply("ok"));
    EXPECT_TRUE(r.is_stream_route(HttpMethod::POST, "/upload"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::PUT, "/upload"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::POST, "/other"));
    // Still an ordinary route for dispatch.
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::POST, "/upload")), "ok");
}

TEST(RouterPathsTest, DynamicStreamRoutesMatchBySegments) {
    Router r;
    r.add_stream_route(HttpMethod::PUT, "/files/:name/content", reply("ok"));
    EXPECT_TRUE(r.is_stream_route(HttpMethod::PUT, "/files/report.pdf/content"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::PUT, "/files/report.pdf"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::PUT, "/files/report.pdf/meta"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::POST, "/files/report.pdf/content"));
}

TEST(RouterPathsTest, StreamRouteInAGroupIsVisibleFromTheGroup) {
    Router r;
    r.group("/v1", [](Router& g) {
        g.add_stream_route(HttpMethod::POST, "/ingest", reply("ok"));
        EXPECT_TRUE(g.is_stream_route(HttpMethod::POST, "/v1/ingest"));
    });
    EXPECT_TRUE(r.is_stream_route(HttpMethod::POST, "/v1/ingest"));
}

// --- Fluent builder ---

TEST(RouterPathsTest, RouteBuilderRegistersTheHandler) {
    Router r;
    r.route("/docs", HttpMethod::POST).summary("Create").tag("docs").handler(reply("created"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::POST, "/docs")), "created");
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/docs")->last().status_code, HttpStatus::MethodNotAllowed);
}

// --- Wildcards, mount-path middleware and not_found (#189) ---

namespace {

// Replies with the given label followed by the request's params, sorted.
RouteHandler echo_params(std::string label) {
    return [label](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        std::vector<std::string> parts;
        for (const auto& [k, v] : req.params) parts.push_back(k + "=" + v);
        std::sort(parts.begin(), parts.end());
        std::string body = label;
        for (const auto& p : parts) body += " " + p;
        HttpResponse res;
        res.send(body);
        w->send(std::move(res));
    };
}

} // namespace

TEST(RouterWildcardTest, TrailingWildcardCapturesTheRest) {
    Router r;
    r.get("/files/*", echo_params("files"));
    r.get("/assets/*rest", echo_params("assets"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/a")), "files *=a");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/a/b/c.txt")), "files *=a/b/c.txt");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files")), "files *=") << "zero segments match too";
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/assets/css/site.css")), "assets rest=css/site.css");
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/filesx/a")->last().status_code, HttpStatus::NotFound);
}

TEST(RouterWildcardTest, ExactThenParamThenMostSpecificWildcard) {
    Router r;
    r.get("/files/*", echo_params("any"));
    r.get("/files/img/*name", echo_params("img"));
    r.get("/files/:id", echo_params("param"));
    r.get("/files/static", echo_params("exact"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/static")), "exact");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/42")), "param id=42");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/img/a.png")), "img name=a.png");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/files/docs/a/b")), "any *=docs/a/b");
}

TEST(RouterWildcardTest, WildcardWithParamsBeforeIt) {
    Router r;
    r.get("/users/:id/files/*path", echo_params("user-files"));
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/users/7/files/x/y")), "user-files id=7 path=x/y");
}

TEST(RouterWildcardTest, WildcardMustBeLast) {
    Router r;
    EXPECT_THROW(r.get("/a/*/b", reply("x")), std::invalid_argument);
    EXPECT_THROW(r.get("/a/*rest/b", reply("x")), std::invalid_argument);
}

TEST(RouterWildcardTest, WildcardInGroupsAndMethodChecks) {
    Router r;
    r.group("/api", [](Router& api) { api.get("/v1/*", echo_params("v1")); });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/v1/a/b")), "v1 *=a/b");
    auto wrong = dispatch(r, HttpMethod::POST, "/api/v1/a");
    EXPECT_EQ(wrong->last().status_code, HttpStatus::MethodNotAllowed);
    EXPECT_EQ(wrong->last().headers.at("Allow"), "GET, HEAD");
}

TEST(RouterWildcardTest, StreamRoutesMatchWildcards) {
    Router r;
    r.add_stream_route(HttpMethod::POST, "/upload/*", reply("ok"));
    EXPECT_TRUE(r.is_stream_route(HttpMethod::POST, "/upload/a/b"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::POST, "/uploadx/a"));
    EXPECT_FALSE(r.is_stream_route(HttpMethod::GET, "/upload/a"));
}

TEST(RouterMountTest, PrefixMiddlewareRunsOnlyUnderItsPrefix) {
    Router r;
    std::vector<std::string> seen;
    r.use("/svc", [&seen](HttpRequest& req, std::shared_ptr<ResponseWriter>) {
        seen.push_back(req.uri);
        return true;
    });
    r.get("/svc/a", reply("a"));
    r.get("/svcx", reply("x"));
    r.get("/other", reply("o"));
    dispatch(r, HttpMethod::GET, "/svc/a");
    dispatch(r, HttpMethod::GET, "/svc");
    dispatch(r, HttpMethod::GET, "/svcx");
    dispatch(r, HttpMethod::GET, "/other");
    EXPECT_EQ(seen, (std::vector<std::string>{"/svc/a", "/svc"}));
}

TEST(RouterMountTest, PrefixMiddlewareCanAnswerPathsWithoutRoutes) {
    Router r;
    r.use("/proxy/", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send("proxied " + req.uri);
        w->send(std::move(res));
        return false;
    });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/proxy/anything/at/all")), "proxied /proxy/anything/at/all");
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/elsewhere")->last().status_code, HttpStatus::NotFound);
    EXPECT_THROW(r.use("relative", [](HttpRequest&, std::shared_ptr<ResponseWriter>) { return true; }),
                 std::invalid_argument);
}

TEST(RouterMountTest, GroupPrefixesCompose) {
    Router r;
    int hits = 0;
    r.group("/g", [&hits](Router& g) {
        g.use("/inner", [&hits](HttpRequest&, std::shared_ptr<ResponseWriter>) { ++hits; return true; });
        g.get("/inner/x", reply("ix"));
        g.get("/outer", reply("o"));
    });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/g/inner/x")), "ix");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/g/outer")), "o");
    EXPECT_EQ(hits, 1);
}

TEST(RouterNotFoundTest, CustomHandlerAnswersUnknownPathsButNot405) {
    Router r;
    r.get("/known", reply("k"));
    r.not_found([](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.status(HttpStatus::NotFound).json(nlohmann::json{{"error", "no route"}, {"path", req.uri}});
        w->send(std::move(res));
    });
    auto missing = dispatch(r, HttpMethod::GET, "/nope");
    EXPECT_EQ(missing->last().status_code, HttpStatus::NotFound);
    EXPECT_NE(missing->last().body.find("\"path\":\"/nope\""), std::string::npos) << missing->last().body;
    EXPECT_EQ(dispatch(r, HttpMethod::POST, "/known")->last().status_code, HttpStatus::MethodNotAllowed);
    // Registered inside a group, it is still the app's handler.
    Router r2;
    r2.group("/g", [](Router& g) { g.not_found(reply("group-404")); });
    EXPECT_EQ(body_of(dispatch(r2, HttpMethod::GET, "/anything")), "group-404");
}

// The registry directly, not through App: App.hpp brings in <windows.h> on
// Windows, whose DELETE macro breaks HttpMethod::DELETE in this file.
TEST(RouterWildcardTest, OpenApiShowsWildcardsAsPathParameters) {
    orbit::openapi::OpenApiRegistry registry;
    registry.register_route(HttpMethod::GET, "/files/*path", {});
    registry.register_route(HttpMethod::GET, "/assets/*", {});
    registry.register_route(HttpMethod::GET, "/users/:id", {});
    std::string spec = registry.generate_swagger_json("t", "1");
    EXPECT_NE(spec.find("\"/files/{path}\""), std::string::npos) << spec;
    EXPECT_NE(spec.find("\"/assets/{path}\""), std::string::npos) << spec;
    EXPECT_NE(spec.find("\"/users/{id}\""), std::string::npos) << spec;
    EXPECT_EQ(spec.find('*'), std::string::npos) << "no literal * in the spec: " << spec;
}

// --- Typed route segments (#201) ---

namespace {

// Replies with every captured param, "name=value" one per line, so a test
// can see exactly what the router extracted.
RouteHandler echo_params() {
    return [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        std::vector<std::string> pairs;
        for (const auto& [k, v] : req.params) pairs.push_back(k + "=" + v);
        std::sort(pairs.begin(), pairs.end());
        std::string body;
        for (auto& p : pairs) body += p + "\n";
        HttpResponse res;
        res.send(body);
        w->send(std::move(res));
    };
}

} // namespace

TEST(RouterTypedSegmentsTest, UntypedBracesMatchAnyNonEmptySegment) {
    Router r;
    r.get("/tasks/{id}", echo_params());
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/tasks/anything-at-all")), "id=anything-at-all\n");
}

TEST(RouterTypedSegmentsTest, IntConstraintAcceptsOnlyIntegers) {
    Router r;
    r.get("/tasks/{id:int}", echo_params());
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/tasks/42")), "id=42\n");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/tasks/-7")), "id=-7\n");
    // Doesn't parse as int: this route doesn't match, so there's no route
    // at all for it here (no not_found handler registered) -> 404.
    auto bad = dispatch(r, HttpMethod::GET, "/tasks/abc");
    EXPECT_EQ(bad->last().status_code, HttpStatus::NotFound);
    auto decimal = dispatch(r, HttpMethod::GET, "/tasks/4.2");
    EXPECT_EQ(decimal->last().status_code, HttpStatus::NotFound);
}

// {id:int} accepts the type's whole 64-bit range, not only what fits a
// (typically 32-bit) C++ int: req.param<T> for the route-matched value
// needs a T at least as wide (int64_t) to never see nullopt (#254 review).
TEST(RouterTypedSegmentsTest, IntConstraintAcceptsValuesOutside32BitRange) {
    Router r;
    r.get("/tasks/{id:int}", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        auto narrow = req.param<int>("id");
        auto wide = req.param<std::int64_t>("id");
        res.send((narrow ? "int:" + std::to_string(*narrow) : std::string("int:nullopt")) + " " +
                 (wide ? "int64_t:" + std::to_string(*wide) : std::string("int64_t:nullopt")));
        w->send(std::move(res));
    });
    auto res = dispatch(r, HttpMethod::GET, "/tasks/99999999999"); // the route matches: fits a 64-bit "int"
    EXPECT_EQ(res->last().status_code, HttpStatus::OK);
    EXPECT_EQ(body_of(res), "int:nullopt int64_t:99999999999");
}

TEST(RouterTypedSegmentsTest, UuidConstraintAcceptsOnlyCanonicalUuids) {
    Router r;
    r.get("/widgets/{id:uuid}", echo_params());
    const std::string uuid = "123e4567-e89b-12d3-a456-426614174000";
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/widgets/" + uuid)), "id=" + uuid + "\n");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/widgets/123E4567-E89B-12D3-A456-426614174000")),
              "id=123E4567-E89B-12D3-A456-426614174000\n")
        << "hex digits are matched case-insensitively";
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/widgets/not-a-uuid")->last().status_code, HttpStatus::NotFound);
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/widgets/42")->last().status_code, HttpStatus::NotFound);
}

// str is the same as no type at all: any non-empty segment.
TEST(RouterTypedSegmentsTest, StrConstraintIsTheDefault) {
    Router r;
    r.get("/pages/{slug:str}", echo_params());
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/pages/hello-world")), "slug=hello-world\n");
}

// A type mismatch on one route falls through to the next candidate, as an
// unmatched literal segment already does.
TEST(RouterTypedSegmentsTest, TypeMismatchFallsThroughToTheNextRoute) {
    Router r;
    r.get("/items/{id:int}", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send("numeric");
        w->send(std::move(res));
    });
    r.get("/items/:slug", [](HttpRequest& req, std::shared_ptr<ResponseWriter> w) {
        HttpResponse res;
        res.send("slug:" + req.params["slug"]);
        w->send(std::move(res));
    });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/items/42")), "numeric");
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/items/widget")), "slug:widget");
}

TEST(RouterTypedSegmentsTest, UnknownTypeThrowsAtRegistration) {
    Router r;
    EXPECT_THROW(r.get("/x/{id:money}", reply("x")), std::invalid_argument);
}

TEST(RouterTypedSegmentsTest, EmptyNameThrowsAtRegistration) {
    Router r;
    EXPECT_THROW(r.get("/x/{:int}", reply("x")), std::invalid_argument);
    EXPECT_THROW(r.get("/x/{}", reply("x")), std::invalid_argument);
}

TEST(RouterTypedSegmentsTest, TypedSegmentWorksInAGroup) {
    Router r;
    r.group("/api", [](Router& api) { api.get("/tasks/{id:int}", echo_params()); });
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/api/tasks/7")), "id=7\n");
    EXPECT_EQ(dispatch(r, HttpMethod::GET, "/api/tasks/nope")->last().status_code, HttpStatus::NotFound);
}

TEST(RouterTypedSegmentsTest, OpenApiShowsTypedSegmentsWithoutTheirType) {
    orbit::openapi::OpenApiRegistry registry;
    registry.register_route(HttpMethod::GET, "/tasks/{id:int}", {});
    registry.register_route(HttpMethod::GET, "/widgets/{id:uuid}", {});
    std::string spec = registry.generate_swagger_json("t", "1");
    EXPECT_NE(spec.find("\"/tasks/{id}\""), std::string::npos) << spec;
    EXPECT_NE(spec.find("\"/widgets/{id}\""), std::string::npos) << spec;
    EXPECT_EQ(spec.find(":int"), std::string::npos) << spec;
    EXPECT_EQ(spec.find(":uuid"), std::string::npos) << spec;
}

// The router dispatches /tasks/{id:int} and /tasks/{id:uuid} separately
// (different patterns, tried in order), but OpenAPI has no way to express
// two shapes of the same {id}, so they collapse to one published operation
// (the most recently registered) with a warning logged (#254 review).
TEST(RouterTypedSegmentsTest, DifferentlyTypedSegmentsCollapseInOpenApiWithAWarning) {
    orbit::openapi::OpenApiRegistry registry;
    orbit::openapi::RouteMetadata int_meta;
    int_meta.summary = "numeric id";
    orbit::openapi::RouteMetadata uuid_meta;
    uuid_meta.summary = "uuid id";

    testing::internal::CaptureStdout();
    registry.register_route(HttpMethod::GET, "/tasks/{id:int}", int_meta);
    registry.register_route(HttpMethod::GET, "/tasks/{id:uuid}", uuid_meta);
    std::string log = testing::internal::GetCapturedStdout();
    EXPECT_NE(log.find("/tasks/{id:uuid}"), std::string::npos) << log;
    EXPECT_NE(log.find("/tasks/{id}"), std::string::npos) << log;

    std::string spec = registry.generate_swagger_json("t", "1");
    EXPECT_NE(spec.find("uuid id"), std::string::npos) << "the later registration's metadata is kept: " << spec;
    EXPECT_EQ(spec.find("numeric id"), std::string::npos) << spec;
}

// --- Router::validate_routes() (#201) ---

TEST(RouterValidateRoutesTest, CleanRouterHasNoProblems) {
    Router r;
    r.get("/a", reply("a"));
    r.post("/a", reply("a-post"));
    r.get("/b/:id", reply("b"));
    EXPECT_TRUE(r.validate_routes().empty());
}

TEST(RouterValidateRoutesTest, DuplicateStaticRouteIsReported) {
    Router r;
    r.get("/dup", reply("1"));
    r.get("/dup", reply("2"));
    auto problems = r.validate_routes();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0], "GET /dup registered 2 times");
    // The later registration wins, as before (validate_routes only reports;
    // it does not itself change matching behaviour).
    EXPECT_EQ(body_of(dispatch(r, HttpMethod::GET, "/dup")), "2");
}

TEST(RouterValidateRoutesTest, SameTextRegisteredThreeTimesCountsAllThree) {
    Router r;
    r.get("/x", reply("1"));
    r.get("/x", reply("2"));
    r.get("/x", reply("3"));
    auto problems = r.validate_routes();
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0], "GET /x registered 3 times");
}

TEST(RouterValidateRoutesTest, MultipleDuplicatesAreSortedForStableOutput) {
    Router r;
    r.post("/z", reply("1"));
    r.post("/z", reply("2"));
    r.get("/a", reply("1"));
    r.get("/a", reply("2"));
    auto problems = r.validate_routes();
    ASSERT_EQ(problems.size(), 2u);
    EXPECT_EQ(problems[0], "GET /a registered 2 times");
    EXPECT_EQ(problems[1], "POST /z registered 2 times");
}
