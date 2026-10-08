#include <gtest/gtest.h>
#include <orbit/server/App.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include "../utils/TestConfig.hpp"

#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Exceptions escaping coroutine handlers (#184). They used to reach
// Task::promise_type::unhandled_exception(), which called std::terminate()
// and took the whole server down. Now they get the same handling as a
// synchronous handler's exception: on_error, else a 500.

using namespace orbit::http;
using orbit::concurrency::Task;

namespace {

constexpr int kPort = 8171;

// Resumes the coroutine on another thread, as database and HTTP client
// callbacks do: the exception is thrown far from the router's try/catch.
struct ResumeElsewhere {
    std::chrono::milliseconds delay{5};
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const {
        std::thread([h, d = delay] {
            std::this_thread::sleep_for(d);
            h.resume();
        }).detach();
    }
    void await_resume() const noexcept {}
};

std::atomic<int> helper_finished{0};

// A coroutine that is not a handler (no ResponseWriter parameter).
Task background_helper() {
    co_await ResumeElsewhere{};
    helper_finished = 1;
    throw std::runtime_error("helper failed");
}

size_t collect(void* data, size_t size, size_t n, void* out) {
    static_cast<std::string*>(out)->append(static_cast<char*>(data), size * n);
    return size * n;
}

struct Result {
    long status = 0;
    std::string body;
    long new_connections = -1;
    CURLcode code = CURLE_OK;
};

Result get(CURL* curl, const std::string& path) {
    Result r;
    std::string url = "http://127.0.0.1:" + std::to_string(kPort) + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    r.code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &r.new_connections);
    return r;
}

void reply(std::shared_ptr<ResponseWriter>& w, const std::string& body) {
    HttpResponse res;
    res.set_body(body, "text/plain");
    w->send(std::move(res));
}

} // namespace

class CoroutineErrorsE2ETest : public ::testing::Test {
protected:
    static std::unique_ptr<orbit::server::App> app;
    static std::thread server_thread;
    CURL* curl = nullptr;

    static void SetUpTestSuite() {
        orbit::config::ServerConfig cfg = orbit::test::server_config();
        cfg.host = "127.0.0.1";
        cfg.port = kPort;
        app = std::make_unique<orbit::server::App>(cfg);

        // "handled" is answered by on_error; anything else makes on_error
        // throw too, which must still end in a 500.
        app->on_error([](const std::exception& e, HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            if (std::string(e.what()) == "handled") {
                HttpResponse res;
                res.status(HttpStatus::BadRequest).set_body("handled: " + std::string(e.what()), "text/plain");
                w->send(std::move(res));
                return;
            }
            throw std::runtime_error("not handled here");
        });

        app->get("/ok", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) { reply(w, "ok"); });

        app->get("/before-suspend", [](HttpRequest&, std::shared_ptr<ResponseWriter>) -> Task {
            co_await std::suspend_never{}; // a coroutine, but still running inside route()
            throw std::runtime_error("before the first co_await");
        });
        app->get("/after-suspend", [](HttpRequest&, std::shared_ptr<ResponseWriter>) -> Task {
            co_await ResumeElsewhere{};
            throw std::runtime_error("after a co_await, on another thread");
        });
        app->get("/handled", [](HttpRequest&, std::shared_ptr<ResponseWriter>) -> Task {
            co_await ResumeElsewhere{};
            throw std::runtime_error("handled");
        });
        app->get("/after-response", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) -> Task {
            reply(w, "sent first");
            co_await ResumeElsewhere{std::chrono::milliseconds(50)};
            throw std::runtime_error("after the response was sent");
        });
        app->get("/helper", [](HttpRequest&, std::shared_ptr<ResponseWriter> w) {
            background_helper();
            reply(w, "helper started");
        });
        // Group routes take the handler as a plain std::function, without
        // App's handler templates in between.
        app->group("/group", [](orbit::routing::Router& r) {
            r.get("/after-suspend", [](HttpRequest&, std::shared_ptr<ResponseWriter>) -> Task {
                co_await ResumeElsewhere{};
                throw std::runtime_error("in a group route");
            });
        });

        server_thread = std::thread([] { app->listen(); });
        CURL* probe = curl_easy_init();
        for (int i = 0; i < 100 && get(probe, "/ok").status != 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        curl_easy_cleanup(probe);
    }

    static void TearDownTestSuite() {
        app->stop();
        server_thread.join();
        app.reset();
    }

    void SetUp() override { curl = curl_easy_init(); }
    void TearDown() override { curl_easy_cleanup(curl); }
};

std::unique_ptr<orbit::server::App> CoroutineErrorsE2ETest::app;
std::thread CoroutineErrorsE2ETest::server_thread;

TEST_F(CoroutineErrorsE2ETest, ThrowBeforeFirstSuspensionIs500) {
    auto r = get(curl, "/before-suspend");
    EXPECT_EQ(r.code, CURLE_OK);
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(r.body.find("co_await"), std::string::npos) << "exception text must not reach the client";
}

TEST_F(CoroutineErrorsE2ETest, ThrowAfterSuspensionIs500AndServerSurvives) {
    auto r = get(curl, "/after-suspend");
    EXPECT_EQ(r.code, CURLE_OK);
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(get(curl, "/ok").status, 200) << "the server must keep serving";
}

TEST_F(CoroutineErrorsE2ETest, OnErrorHandlesAsyncExceptions) {
    auto r = get(curl, "/handled");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body, "handled: handled");
}

TEST_F(CoroutineErrorsE2ETest, GroupRouteCoroutineIsHandled) {
    auto r = get(curl, "/group/after-suspend");
    EXPECT_EQ(r.code, CURLE_OK);
    EXPECT_EQ(r.status, 500);
}

TEST_F(CoroutineErrorsE2ETest, ThrowAfterRespondingKeepsTheResponseAndTheConnection) {
    auto first = get(curl, "/after-response");
    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(first.body, "sent first");
    // The next request on the same keep-alive connection arrives while the
    // first coroutine is still suspended; its late exception must not be
    // answered on this request.
    auto second = get(curl, "/ok");
    EXPECT_EQ(second.status, 200);
    EXPECT_EQ(second.body, "ok");
    EXPECT_EQ(second.new_connections, 0) << "the connection should have been reused";
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // let the exception happen
    auto third = get(curl, "/ok");
    EXPECT_EQ(third.status, 200);
    EXPECT_EQ(third.body, "ok");
}

TEST_F(CoroutineErrorsE2ETest, NonHandlerCoroutineExceptionIsLoggedNotFatal) {
    helper_finished = 0;
    EXPECT_EQ(get(curl, "/helper").status, 200);
    for (int i = 0; i < 100 && helper_finished == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(helper_finished.load(), 1);
    EXPECT_EQ(get(curl, "/ok").status, 200) << "the process must still be running";
}
