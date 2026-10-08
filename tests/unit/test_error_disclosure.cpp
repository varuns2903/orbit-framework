#include <gtest/gtest.h>
#include <orbit/routing/Router.hpp>
#include <orbit/routing/HandlerWrapper.hpp>
#include <orbit/http/HttpResponse.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace orbit::http;

namespace {

class CaptureWriter : public ResponseWriter {
public:
    HttpResponse last;
    void send(HttpResponse&& r) override { last = std::move(r); }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("unused"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("unused"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

const char* kSecret = "password=hunter2 at /srv/app/db.cpp:42";

} // namespace

TEST(ErrorDisclosureTest, RouteExceptionTextIsNotSentToClient) {
    orbit::routing::Router router;
    router.get("/boom", [](HttpRequest&, std::shared_ptr<ResponseWriter>) {
        throw std::runtime_error(kSecret);
    });
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.uri = "/boom";
    auto w = std::make_shared<CaptureWriter>();
    router.route(req, w);
    EXPECT_EQ(w->last.status_code, HttpStatus::InternalServerError);
    EXPECT_EQ(w->last.body.find("hunter2"), std::string::npos) << w->last.body;
}

TEST(ErrorDisclosureTest, MagicHandlerExceptionsReachTheErrorHandler) {
    orbit::routing::Router router;
    router.get("/magic", orbit::routing::wrap_handler([]() -> std::string {
        throw std::runtime_error(kSecret);
    }));
    bool handler_called = false;
    router.on_error([&](const std::exception&, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
        handler_called = true;
        HttpResponse res;
        res.status(HttpStatus::InternalServerError).send("custom");
        w->send(std::move(res));
    });
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.uri = "/magic";
    auto w = std::make_shared<CaptureWriter>();
    router.route(req, w);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(w->last.body, "custom");
}

TEST(ErrorDisclosureTest, TemplateErrorsAreNotRendered) {
    auto path = std::filesystem::temp_directory_path() / "orbit_bad_template.html";
    { std::ofstream(path) << "{{ unclosed "; }
    HttpResponse res;
    res.render(path.string(), nlohmann::json::object());
    std::filesystem::remove(path);
    EXPECT_EQ(res.status_code, HttpStatus::InternalServerError);
    EXPECT_EQ(res.body, "<h1>500 Internal Server Error</h1>");
}
