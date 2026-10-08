#include <gtest/gtest.h>
#include <orbit/middleware/Metrics.hpp>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <string>
#include <vector>

using namespace orbit::middleware;
using namespace orbit::http;

namespace {

class MetricsMockResponseWriter : public ResponseWriter {
public:
    std::vector<std::function<void(HttpResponse&)>> interceptors;

    // What the real writer does when the handler sends its response.
    void respond(HttpResponse res) {
        for (auto& i : interceptors) i(res);
    }

    void send(HttpResponse&&) override {}
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)> interceptor) override {
        interceptors.push_back(std::move(interceptor));
    }
    void set_header(const std::string&, const std::string&) override {}
    orbit::network::Proactor& proactor() override { throw std::runtime_error("Not implemented"); }
    orbit::concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("Not implemented"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

// The value of one sample ("name" or "name{labels}") in the registry's
// exposition, or 0 if it is not there yet. The registry is process-wide, so
// tests compare values before and after rather than absolute numbers.
double sample(const std::string& series) {
    std::istringstream in(orbit::utils::PrometheusRegistry::get_instance().expose());
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() > series.size() && line.compare(0, series.size(), series) == 0 &&
            line[series.size()] == ' ') {
            return std::strtod(line.c_str() + series.size() + 1, nullptr);
        }
    }
    return 0;
}

const std::string kInFlight = "orbit_http_requests_in_flight";

} // namespace

TEST(MetricsTest, CountsRequestsByMethodAndStatus) {
    auto m = Metrics::track();
    const std::string series = R"(orbit_http_requests_total{method="POST",status="201"})";
    const double before = sample(series);

    HttpRequest req;
    req.method = HttpMethod::POST;
    auto writer = std::make_shared<MetricsMockResponseWriter>();
    ASSERT_TRUE(m(req, writer));
    HttpResponse res;
    res.status_code = HttpStatus::Created;
    writer->respond(std::move(res));

    EXPECT_EQ(sample(series), before + 1);
}

TEST(MetricsTest, EveryMethodHasItsOwnLabel) {
    auto m = Metrics::track();
    const std::pair<HttpMethod, const char*> methods[] = {
        {HttpMethod::GET, "GET"}, {HttpMethod::POST, "POST"}, {HttpMethod::PUT, "PUT"},
        {HttpMethod::PATCH, "PATCH"}, {HttpMethod::DELETE, "DELETE"}, {HttpMethod::OPTIONS, "OPTIONS"},
        {HttpMethod::HEAD, "HEAD"}, {HttpMethod::UNKNOWN, "UNKNOWN"},
    };
    for (const auto& [method, label] : methods) {
        const std::string series = std::string("orbit_http_requests_total{method=\"") + label + "\",status=\"418\"}";
        const double before = sample(series);
        HttpRequest req;
        req.method = method;
        auto writer = std::make_shared<MetricsMockResponseWriter>();
        ASSERT_TRUE(m(req, writer));
        HttpResponse res;
        res.status_code = static_cast<HttpStatus>(418);
        writer->respond(std::move(res));
        EXPECT_EQ(sample(series), before + 1) << label;
    }
}

TEST(MetricsTest, InFlightGaugeCoversTheRequestUntilItsResponse) {
    auto m = Metrics::track();
    const double before = sample(kInFlight);

    HttpRequest req;
    req.method = HttpMethod::GET;
    auto writer = std::make_shared<MetricsMockResponseWriter>();
    ASSERT_TRUE(m(req, writer));
    EXPECT_EQ(sample(kInFlight), before + 1);

    writer->respond(HttpResponse{});
    EXPECT_EQ(sample(kInFlight), before);
}

TEST(MetricsTest, DurationIsObservedOncePerResponse) {
    auto m = Metrics::track();
    const std::string count = R"(orbit_http_request_duration_seconds_count{method="PATCH"})";
    const std::string inf = R"(orbit_http_request_duration_seconds_bucket{method="PATCH",le="+Inf"})";
    const double before = sample(count);

    HttpRequest req;
    req.method = HttpMethod::PATCH;
    auto writer = std::make_shared<MetricsMockResponseWriter>();
    ASSERT_TRUE(m(req, writer));
    writer->respond(HttpResponse{});

    EXPECT_EQ(sample(count), before + 1);
    EXPECT_EQ(sample(inf), sample(count));
}

TEST(MetricsTest, RequestBytesCountTheBody) {
    auto m = Metrics::track();
    const std::string series = "orbit_http_request_bytes_total";
    const double before = sample(series);

    const std::string body(1500, 'x');
    HttpRequest req;
    req.method = HttpMethod::PUT;
    req.body = body;
    auto writer = std::make_shared<MetricsMockResponseWriter>();
    ASSERT_TRUE(m(req, writer));

    EXPECT_EQ(sample(series), before + 1500);
}

TEST(MetricsTest, ResponseBytesPreferContentLength) {
    auto m = Metrics::track();
    const std::string series = R"(orbit_http_response_bytes_total{method="DELETE"})";
    auto respond = [&](HttpResponse res) {
        HttpRequest req;
        req.method = HttpMethod::DELETE;
        auto writer = std::make_shared<MetricsMockResponseWriter>();
        m(req, writer);
        writer->respond(std::move(res));
    };

    // Content-Length, when set, is what goes on the wire (a HEAD response or
    // a file sent with sendfile has no body in the HttpResponse).
    double before = sample(series);
    HttpResponse declared;
    declared.headers["Content-Length"] = "4096";
    respond(std::move(declared));
    EXPECT_EQ(sample(series), before + 4096);

    // No Content-Length: the body size.
    before = sample(series);
    HttpResponse plain;
    plain.body = std::string(300, 'y');
    respond(std::move(plain));
    EXPECT_EQ(sample(series), before + 300);

    // An unparsable Content-Length falls back to the body size.
    before = sample(series);
    HttpResponse garbled;
    garbled.body = std::string(7, 'z');
    garbled.headers["Content-Length"] = "lots";
    respond(std::move(garbled));
    EXPECT_EQ(sample(series), before + 7);
}

// A WebSocket upgrade never sends an HttpResponse through the writer, so
// counting it would leave it in flight forever.
TEST(MetricsTest, UpgradeRequestsAreNotTracked) {
    auto m = Metrics::track();
    const double in_flight = sample(kInFlight);
    const double bytes = sample("orbit_http_request_bytes_total");

    const std::string body = "ignored";
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.set_header("Upgrade", "websocket");
    req.body = body;
    auto writer = std::make_shared<MetricsMockResponseWriter>();
    EXPECT_TRUE(m(req, writer));

    EXPECT_TRUE(writer->interceptors.empty());
    EXPECT_EQ(sample(kInFlight), in_flight);
    EXPECT_EQ(sample("orbit_http_request_bytes_total"), bytes);
}

// --- Exposition format ---

TEST(PrometheusRegistryTest, LargeValuesKeepEveryDigit) {
    auto& reg = orbit::utils::PrometheusRegistry::get_instance();
    reg.set_gauge("orbit_test_large_gauge", "", 1234567.0);
    EXPECT_EQ(sample("orbit_test_large_gauge"), 1234567.0);

    reg.set_gauge("orbit_test_large_gauge", "", 9007199254740991.0); // 2^53 - 1
    EXPECT_EQ(sample("orbit_test_large_gauge"), 9007199254740991.0);

    reg.observe_histogram("orbit_test_large_histogram", "", 2500000.5);
    EXPECT_EQ(sample("orbit_test_large_histogram_sum"), 2500000.5);
}

TEST(PrometheusRegistryTest, FractionsReadBackExactly) {
    auto& reg = orbit::utils::PrometheusRegistry::get_instance();
    reg.set_gauge("orbit_test_fraction_gauge", "", 0.1);
    EXPECT_EQ(sample("orbit_test_fraction_gauge"), 0.1);
    reg.set_gauge("orbit_test_fraction_gauge", "", 123456.789012345);
    EXPECT_EQ(sample("orbit_test_fraction_gauge"), 123456.789012345);
}

TEST(PrometheusRegistryTest, NonFiniteValuesUseThePrometheusSpelling) {
    auto& reg = orbit::utils::PrometheusRegistry::get_instance();
    reg.set_gauge("orbit_test_nonfinite", "kind=\"pos\"", std::numeric_limits<double>::infinity());
    reg.set_gauge("orbit_test_nonfinite", "kind=\"neg\"", -std::numeric_limits<double>::infinity());
    reg.set_gauge("orbit_test_nonfinite", "kind=\"nan\"", std::numeric_limits<double>::quiet_NaN());
    const std::string text = reg.expose();
    EXPECT_NE(text.find("orbit_test_nonfinite{kind=\"pos\"} +Inf\n"), std::string::npos) << text;
    EXPECT_NE(text.find("orbit_test_nonfinite{kind=\"neg\"} -Inf\n"), std::string::npos) << text;
    EXPECT_NE(text.find("orbit_test_nonfinite{kind=\"nan\"} NaN\n"), std::string::npos) << text;
}
