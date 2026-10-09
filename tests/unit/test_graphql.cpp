#include <gtest/gtest.h>
#include <orbit/middleware/GraphQL.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The GraphQL middleware's responses are valid JSON whatever the executor
// returns or throws (#188).

using namespace orbit::http;

namespace {

class CaptureWriter : public ResponseWriter {
public:
    std::vector<HttpResponse> sent;
    void send(HttpResponse&& response) override {
        mark_responded();
        sent.push_back(std::move(response));
    }
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

// Runs one POST through the middleware and returns the parsed response body.
nlohmann::json post(orbit::middleware::GraphQLExecutor executor, const std::string& body, HttpStatus* status) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.uri = "/graphql";
    req.set_header("Content-Type", "application/json");
    std::string stored = body;
    req.body = stored;
    auto writer = std::make_shared<CaptureWriter>();
    orbit::middleware::graphql(std::move(executor))(req, writer);
    EXPECT_EQ(writer->sent.size(), 1u);
    if (writer->sent.empty()) return nullptr;
    *status = writer->sent.back().status_code;
    auto parsed = nlohmann::json::parse(writer->sent.back().body, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded()) << "not JSON: " << writer->sent.back().body;
    return parsed;
}

} // namespace

TEST(GraphQLTest, ErrorMessagesWithSpecialCharactersStayValidJson) {
    const std::string message = "resolver failed on \"fail\" field\\ with a\nnewline and a\ttab";
    HttpStatus status{};
    auto body = post([&](const std::string&, const std::string&, const nlohmann::json&) -> nlohmann::json {
        throw std::runtime_error(message);
    }, R"({"query":"{ fail }"})", &status);
    EXPECT_EQ(status, HttpStatus::InternalServerError);
    ASSERT_TRUE(body.contains("errors"));
    ASSERT_TRUE(body["errors"].is_array());
    ASSERT_EQ(body["errors"].size(), 1u);
    EXPECT_EQ(body["errors"][0]["message"], message);
}

TEST(GraphQLTest, MissingQueryIsA400WithAnErrorsArray) {
    HttpStatus status{};
    auto body = post([](const std::string&, const std::string&, const nlohmann::json&) { return nlohmann::json{}; },
                     R"({"variables":{}})", &status);
    EXPECT_EQ(status, HttpStatus::BadRequest);
    ASSERT_TRUE(body["errors"].is_array());
    EXPECT_EQ(body["errors"][0]["message"], "GraphQL query is missing");
}

TEST(GraphQLTest, ResultIsReturnedAsJson) {
    HttpStatus status{};
    auto body = post([](const std::string& query, const std::string&, const nlohmann::json& vars) {
        return nlohmann::json{{"data", {{"query", query}, {"id", vars.value("id", 0)}}}};
    }, R"({"query":"{ hello }","variables":{"id":7}})", &status);
    EXPECT_EQ(status, HttpStatus::OK);
    EXPECT_EQ(body["data"]["query"], "{ hello }");
    EXPECT_EQ(body["data"]["id"], 7);
}
