#include <gtest/gtest.h>
#include <orbit/middleware/Validation.hpp>
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <orbit/http/ResponseWriter.hpp>
#include <memory>
#include <optional>
#include <string>

using namespace middleware;
using namespace http;

namespace {

class ValidationMockResponseWriter : public ResponseWriter {
public:
    std::optional<HttpResponse> sent;

    void send(HttpResponse&& response) override { sent = std::move(response); }
    void send_headers(HttpResponse&) override {}
    void write_chunk(std::string_view) override {}
    void end() override {}
    void add_interceptor(std::function<void(HttpResponse&)>) override {}
    void set_header(const std::string&, const std::string&) override {}
    network::Proactor& proactor() override { throw std::runtime_error("Not implemented"); }
    concurrency::ThreadPool& thread_pool() override { throw std::runtime_error("Not implemented"); }
    void send_sse_event(std::string_view, std::string_view, std::string_view) override {}
    void upgrade_to_raw_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
    void read_body_stream(std::function<void(std::string_view)>, std::function<void()>) override {}
};

struct Outcome {
    bool continued;
    std::optional<HttpResponse> response;
};

// HttpRequest::body is a view, so the caller's string must outlive the call.
Outcome run(const std::vector<SchemaField>& schema, const std::string& body) {
    auto m = validate_json(schema);
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.body = body;
    auto writer = std::make_shared<ValidationMockResponseWriter>();
    bool continued = m(req, writer);
    return {continued, std::move(writer->sent)};
}

nlohmann::json details_of(const HttpResponse& res) {
    return nlohmann::json::parse(res.body).at("details");
}

const std::vector<SchemaField> kUserSchema = {
    {"name", JsonType::STRING},
    {"age", JsonType::NUMBER},
    {"nickname", JsonType::STRING, false},
};

} // namespace

TEST(ValidationTest, ValidBodyContinuesWithoutResponding) {
    const std::string body = R"({"name": "Ada", "age": 36})";
    auto out = run(kUserSchema, body);
    EXPECT_TRUE(out.continued);
    EXPECT_FALSE(out.response.has_value());
}

TEST(ValidationTest, ParsedBodyIsCachedOnTheRequest) {
    const std::string body = R"({"name": "Ada", "age": 36})";
    auto m = validate_json(kUserSchema);
    HttpRequest req;
    req.body = body;
    auto writer = std::make_shared<ValidationMockResponseWriter>();
    ASSERT_TRUE(m(req, writer));
    EXPECT_EQ(req.json_body["name"], "Ada");
    EXPECT_EQ(req.json_body["age"], 36);
}

TEST(ValidationTest, UnknownFieldsAreAllowed) {
    const std::string body = R"({"name": "Ada", "age": 36, "extra": [1, 2]})";
    EXPECT_TRUE(run(kUserSchema, body).continued);
}

TEST(ValidationTest, OptionalFieldMayBeAbsentButIsTypeCheckedWhenPresent) {
    const std::string absent = R"({"name": "Ada", "age": 36})";
    EXPECT_TRUE(run(kUserSchema, absent).continued);

    const std::string wrong = R"({"name": "Ada", "age": 36, "nickname": 7})";
    auto out = run(kUserSchema, wrong);
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(details_of(*out.response), nlohmann::json::array({"Invalid type for field 'nickname'"}));
}

TEST(ValidationTest, MissingRequiredFieldIsRejectedWith422) {
    const std::string body = R"({"name": "Ada"})";
    auto out = run(kUserSchema, body);
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(out.response->status_code, HttpStatus::UnprocessableEntity);
    EXPECT_EQ(out.response->headers["Content-Type"], "application/json");

    auto j = nlohmann::json::parse(out.response->body);
    EXPECT_EQ(j["error"], "Validation failed");
    EXPECT_EQ(j["details"], nlohmann::json::array({"Missing required field: age"}));
}

TEST(ValidationTest, EveryErrorIsReportedInSchemaOrder) {
    const std::string body = R"({"name": 42})";
    auto out = run(kUserSchema, body);
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(details_of(*out.response), nlohmann::json::array({
        "Invalid type for field 'name'",
        "Missing required field: age",
    }));
}

TEST(ValidationTest, EachTypeAcceptsOnlyItsJsonKind) {
    struct Case { JsonType type; const char* good; const char* bad; };
    const Case cases[] = {
        {JsonType::STRING,  R"("s")",      "1"},
        {JsonType::NUMBER,  "1.5",         R"("1.5")"},
        {JsonType::BOOLEAN, "true",        "1"},
        {JsonType::OBJECT,  R"({"a": 1})", "[1]"},
        {JsonType::ARRAY,   "[1]",         R"({"a": 1})"},
    };
    for (const auto& c : cases) {
        const std::vector<SchemaField> schema = {{"v", c.type}};
        const std::string good = std::string(R"({"v": )") + c.good + "}";
        const std::string bad = std::string(R"({"v": )") + c.bad + "}";
        EXPECT_TRUE(run(schema, good).continued) << good;
        EXPECT_FALSE(run(schema, bad).continued) << bad;
    }
}

TEST(ValidationTest, NullDoesNotSatisfyARequiredField) {
    const std::vector<SchemaField> schema = {{"v", JsonType::STRING}};
    const std::string body = R"({"v": null})";
    auto out = run(schema, body);
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(details_of(*out.response), nlohmann::json::array({"Invalid type for field 'v'"}));
}

TEST(ValidationTest, MalformedJsonIsRejectedWith422) {
    const std::string body = R"({"name": "Ada",)";
    auto out = run(kUserSchema, body);
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(out.response->status_code, HttpStatus::UnprocessableEntity);
    EXPECT_EQ(out.response->headers["Content-Type"], "application/json");
    EXPECT_EQ(nlohmann::json::parse(out.response->body)["error"], "Invalid JSON payload");
}

TEST(ValidationTest, NonObjectJsonIsRejected) {
    for (const std::string body : {"[1, 2]", R"("text")", "42", "null"}) {
        auto out = run(kUserSchema, body);
        EXPECT_FALSE(out.continued) << body;
        ASSERT_TRUE(out.response.has_value()) << body;
        EXPECT_EQ(nlohmann::json::parse(out.response->body)["error"], "Invalid JSON payload") << body;
    }
}

// An empty body parses as an empty object, so it fails on the required
// fields rather than as malformed JSON.
TEST(ValidationTest, EmptyBodyReportsMissingRequiredFields) {
    auto out = run(kUserSchema, "");
    EXPECT_FALSE(out.continued);
    ASSERT_TRUE(out.response.has_value());
    EXPECT_EQ(details_of(*out.response), nlohmann::json::array({
        "Missing required field: name",
        "Missing required field: age",
    }));

    const std::vector<SchemaField> all_optional = {{"v", JsonType::STRING, false}};
    EXPECT_TRUE(run(all_optional, "").continued);
}
