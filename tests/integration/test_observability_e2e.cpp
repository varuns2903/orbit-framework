#include <gtest/gtest.h>
#include <orbit/middleware/Metrics.hpp>
#include <orbit/middleware/Observability.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <orbit/server/App.hpp>
#include <orbit/utils/Logger.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kPort = 8133;

std::string exchange(const std::string& request) {
    network::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        network::close_socket(fd);
        return "";
    }
#ifdef _WIN32
    DWORD t = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    ::send(fd, request.data(), static_cast<int>(request.size()), 0);
    std::string out;
    char buf[8192];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    network::close_socket(fd);
    return out;
}

std::string get(const std::string& path, const std::string& extra_headers = "") {
    return exchange("GET " + path + " HTTP/1.1\r\nHost: x\r\nUser-Agent: obs-test\r\n" + extra_headers +
                    "Connection: close\r\n\r\n");
}

std::string header_value(const std::string& response, const std::string& name) {
    size_t at = response.find("\r\n" + name + ": ");
    if (at == std::string::npos) return "";
    size_t start = at + name.size() + 4;
    return response.substr(start, response.find("\r\n", start) - start);
}

bool is_hex(const std::string& s, size_t len) {
    return s.size() == len && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::mutex g_mutex;
std::vector<std::string> g_lines;
std::vector<middleware::Span> g_spans;

} // namespace

class ObservabilityTest : public ::testing::Test {
protected:
    static server::App* app;
    static std::thread server_thread;

    static void SetUpTestSuite() {
        utils::Logger::set_format(utils::LogFormat::Json);
        utils::Logger::set_sink([](const std::string& line) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_lines.push_back(line);
        });

        config::ServerConfig cfg;
        cfg.port = kPort;
        cfg.log_format = "json";
        app = new server::App(cfg);
        app->use(middleware::request_id());
        middleware::TracingOptions tracing;
        tracing.on_span_end = [](const middleware::Span& span) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_spans.push_back(span);
        };
        app->use(middleware::tracing(tracing));
        app->use(middleware::access_log());
        app->use(middleware::Metrics::track());
        app->enable_metrics();
        app->get("/items", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            res.set_body("items");
            w->send(std::move(res));
        });
        app->get("/stream", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter> w) {
            http::HttpResponse res;
            w->send_headers(res);
            w->write_chunk("a");
            w->end();
        });
        server_thread = std::thread([] { app->listen(); });
        for (int i = 0; i < 200 && get("/items").empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    static void TearDownTestSuite() {
        app->stop();
        if (server_thread.joinable()) server_thread.join();
        delete app;
        utils::Logger::set_sink(nullptr);
        utils::Logger::set_format(utils::LogFormat::Text);
    }

    // Access-log lines containing `needle`.
    static std::vector<std::string> lines_with(const std::string& needle) {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& l : g_lines) {
            if (l.find(needle) != std::string::npos) out.push_back(l);
        }
        return out;
    }
};

server::App* ObservabilityTest::app = nullptr;
std::thread ObservabilityTest::server_thread;

TEST_F(ObservabilityTest, RequestIdIsGeneratedReusedOrReplaced) {
    std::string generated = header_value(get("/items"), "X-Request-ID");
    EXPECT_TRUE(is_hex(generated, 32)) << generated;

    EXPECT_EQ(header_value(get("/items", "X-Request-ID: order-7f3a:retry.1\r\n"), "X-Request-ID"), "order-7f3a:retry.1");

    std::string replaced = header_value(get("/items", "X-Request-ID: <script>\r\n"), "X-Request-ID");
    EXPECT_TRUE(is_hex(replaced, 32)) << "unsafe incoming IDs must not be echoed: " << replaced;
}

TEST_F(ObservabilityTest, TraceparentIsContinuedOrStarted) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_spans.clear();
    }
    get("/items?traced=1", "traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01\r\n");
    get("/items?traced=2");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::lock_guard<std::mutex> lock(g_mutex);
    ASSERT_GE(g_spans.size(), 2u);
    const middleware::Span* continued = nullptr;
    const middleware::Span* root = nullptr;
    for (const auto& s : g_spans) {
        if (s.parent_span_id == "00f067aa0ba902b7") continued = &s;
        else if (s.parent_span_id.empty()) root = &s;
    }
    ASSERT_TRUE(continued);
    EXPECT_EQ(continued->trace_id, "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_TRUE(is_hex(continued->span_id, 16));
    EXPECT_NE(continued->span_id, "00f067aa0ba902b7");
    EXPECT_EQ(continued->status, 200);
    EXPECT_EQ(continued->name, "GET /items");
    ASSERT_TRUE(root);
    EXPECT_TRUE(is_hex(root->trace_id, 32));
    EXPECT_NE(root->trace_id, continued->trace_id);
}

TEST_F(ObservabilityTest, AccessLogIsStructuredJson) {
    get("/items?for=access-log");
    auto lines = lines_with("for=access-log");
    ASSERT_EQ(lines.size(), 1u);
    const std::string& line = lines[0];
    EXPECT_EQ(line.front(), '{');
    EXPECT_NE(line.find("\"level\":\"info\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"method\":\"GET\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"path\":\"/items?for=access-log\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"status\":\"200\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"bytes\":\"5\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"user_agent\":\"obs-test\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"request_id\":\""), std::string::npos) << line;
    EXPECT_NE(line.find("\"trace_id\":\""), std::string::npos) << line;
}

TEST_F(ObservabilityTest, StreamedResponsesAreLoggedToo) {
    get("/stream");
    auto lines = lines_with("\"path\":\"/stream\"");
    ASSERT_EQ(lines.size(), 1u) << "interceptors did not run for a streamed HTTP/1.1 response";
    EXPECT_NE(lines[0].find("\"status\":\"200\""), std::string::npos);
}

TEST_F(ObservabilityTest, MetricsHaveStatusLabelsAndLatencyBuckets) {
    get("/items");
    get("/missing");
    std::string metrics = get("/metrics");
    EXPECT_NE(metrics.find("orbit_http_requests_total{method=\"GET\",status=\"200\"}"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("orbit_http_requests_total{method=\"GET\",status=\"404\"}"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("orbit_http_request_duration_seconds_bucket{method=\"GET\",le=\"0.005\"}"), std::string::npos);
    EXPECT_NE(metrics.find("orbit_http_request_duration_seconds_bucket{method=\"GET\",le=\"+Inf\"}"), std::string::npos);
    EXPECT_NE(metrics.find("orbit_http_requests_in_flight"), std::string::npos);
    EXPECT_NE(metrics.find("orbit_http_response_bytes_total{method=\"GET\"}"), std::string::npos);
}

TEST(LoggerFormatTest, FieldsAreEscapedAndQuoted) {
    std::vector<std::string> lines;
    utils::Logger::set_sink([&lines](const std::string& l) { lines.push_back(l); });

    utils::Logger::set_format(utils::LogFormat::Json);
    utils::Logger::log_fields(utils::LogLevel::ERROR, "dir/file.cpp", 7, "say \"hi\"\n", {{"k", "a\tb"}});
    utils::Logger::set_format(utils::LogFormat::Text);
    utils::Logger::log_fields(utils::LogLevel::ERROR, "dir/file.cpp", 7, "plain", {{"user", "two words"}, {"n", "3"}});

    utils::Logger::set_sink(nullptr);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find(R"("msg":"say \"hi\"\n")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(R"("k":"a\tb")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find(R"("source":"file.cpp:7")"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find("user=\"two words\" n=3"), std::string::npos) << lines[1];
    EXPECT_EQ(lines[1].find("\033["), std::string::npos) << "no colors when writing to a sink";
}
