#include <gtest/gtest.h>
#include <orbit/openapi/OpenApi.hpp>
#include <orbit/http/json.hpp>
#include <string>

using openapi::OpenApiRegistry;
using openapi::RouteMetadata;
using http::HttpMethod;

namespace {

// The spec, parsed; a parse failure fails the test with the text.
nlohmann::json spec_of(const OpenApiRegistry& reg) {
    std::string text = reg.generate_swagger_json("Test API", "2.1.0");
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    EXPECT_FALSE(j.is_discarded()) << "invalid JSON:\n" << text;
    return j;
}

} // namespace

TEST(OpenApiTest, EmptyRegistryIsAValidSpec) {
    OpenApiRegistry reg;
    auto j = spec_of(reg);
    EXPECT_EQ(j["openapi"], "3.0.0");
    EXPECT_EQ(j["info"]["title"], "Test API");
    EXPECT_EQ(j["info"]["version"], "2.1.0");
    EXPECT_TRUE(j["paths"].empty());
}

TEST(OpenApiTest, EveryMethodGetsItsName) {
    OpenApiRegistry reg;
    for (HttpMethod m : {HttpMethod::GET, HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE,
                         HttpMethod::PATCH, HttpMethod::OPTIONS}) {
        reg.register_route(m, "/things", {});
    }
    auto ops = spec_of(reg)["paths"]["/things"];
    for (const char* name : {"get", "post", "put", "delete", "patch", "options"}) {
        EXPECT_TRUE(ops.contains(name)) << name << " in " << ops.dump();
    }
}

TEST(OpenApiTest, PathParametersAreConvertedAndDeclared) {
    OpenApiRegistry reg;
    reg.register_route(HttpMethod::GET, "/users/:user_id/posts/:post_id", {});
    auto paths = spec_of(reg)["paths"];
    ASSERT_TRUE(paths.contains("/users/{user_id}/posts/{post_id}")) << paths.dump();
    auto params = paths["/users/{user_id}/posts/{post_id}"]["get"]["parameters"];
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0]["name"], "user_id");
    EXPECT_EQ(params[0]["in"], "path");
    EXPECT_EQ(params[0]["required"], true);
    EXPECT_EQ(params[1]["name"], "post_id");
}

TEST(OpenApiTest, MetadataTagsBodiesAndResponses) {
    OpenApiRegistry reg;
    RouteMetadata meta;
    meta.summary = "Create a user";
    meta.description = "Creates one";
    meta.tags = {"users", "admin"};
    meta.request_body_schema = "NewUser";
    meta.response_schemas[201] = "User";
    meta.response_schemas[422] = R"({"type": "object", "properties": {"error": {"type": "string"}}})";
    meta.response_schemas[204] = "";
    reg.register_route(HttpMethod::POST, "/users", meta);
    reg.register_schema("User", R"({"type": "object"})");
    reg.register_schema("NewUser", R"({"type": "object", "required": ["name"]})");

    auto j = spec_of(reg);
    auto op = j["paths"]["/users"]["post"];
    EXPECT_EQ(op["summary"], "Create a user");
    EXPECT_EQ(op["description"], "Creates one");
    EXPECT_EQ(op["tags"], nlohmann::json::array({"users", "admin"}));
    EXPECT_EQ(op["requestBody"]["content"]["application/json"]["schema"]["$ref"], "#/components/schemas/NewUser");
    EXPECT_EQ(op["responses"]["201"]["content"]["application/json"]["schema"]["$ref"], "#/components/schemas/User");
    EXPECT_EQ(op["responses"]["422"]["content"]["application/json"]["schema"]["type"], "object");
    // A status registered without a schema has a description and no content.
    EXPECT_TRUE(op["responses"]["204"].contains("description"));
    EXPECT_FALSE(op["responses"]["204"].contains("content"));
    EXPECT_FALSE(op["responses"].contains("200"));

    EXPECT_EQ(j["components"]["schemas"]["User"]["type"], "object");
    EXPECT_EQ(j["components"]["schemas"]["NewUser"]["required"], nlohmann::json::array({"name"}));
}

TEST(OpenApiTest, RoutesWithoutResponsesDefaultTo200) {
    OpenApiRegistry reg;
    reg.register_route(HttpMethod::GET, "/ping", {});
    auto responses = spec_of(reg)["paths"]["/ping"]["get"]["responses"];
    EXPECT_EQ(responses["200"]["description"], "Success");
}

TEST(OpenApiTest, TextIsEscapedIntoValidJson) {
    OpenApiRegistry reg;
    RouteMetadata meta;
    meta.summary = "quote \" backslash \\ newline \n tab \t";
    meta.description = std::string("bell \a, escape \x1b, unit separator \x1f, and \b \f \r");
    meta.tags = {"a\"b"};
    reg.register_route(HttpMethod::GET, "/esc", meta);

    std::string title = "Title with \"quotes\" and \x01";
    std::string text = reg.generate_swagger_json(title, "1\n0");
    auto j = nlohmann::json::parse(text, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << text;
    EXPECT_EQ(j["info"]["title"], title);
    EXPECT_EQ(j["info"]["version"], "1\n0");
    EXPECT_EQ(j["paths"]["/esc"]["get"]["summary"], meta.summary);
    EXPECT_EQ(j["paths"]["/esc"]["get"]["description"], meta.description);
    EXPECT_EQ(j["paths"]["/esc"]["get"]["tags"][0], "a\"b");
}

// Schemas in the process-wide registry appear in every App's spec; an
// App's own schema of the same name wins.
TEST(OpenApiTest, GlobalSchemasAreIncludedAndCanBeOverridden) {
    OpenApiRegistry::instance().register_schema("OrbitTestGlobalOnly", R"({"type": "string"})");
    OpenApiRegistry::instance().register_schema("OrbitTestShared", R"({"type": "string"})");

    OpenApiRegistry reg;
    reg.register_schema("OrbitTestShared", R"({"type": "integer"})");
    auto schemas = spec_of(reg)["components"]["schemas"];
    EXPECT_EQ(schemas["OrbitTestGlobalOnly"]["type"], "string");
    EXPECT_EQ(schemas["OrbitTestShared"]["type"], "integer");

    // The global registry's own spec has its schemas too.
    auto global = spec_of(OpenApiRegistry::instance())["components"]["schemas"];
    EXPECT_EQ(global["OrbitTestShared"]["type"], "string");
}

TEST(OpenApiTest, ReRegisteringAMethodReplacesItsMetadata) {
    OpenApiRegistry reg;
    RouteMetadata first;
    first.summary = "first";
    RouteMetadata second;
    second.summary = "second";
    reg.register_route(HttpMethod::GET, "/x", first);
    reg.register_route(HttpMethod::GET, "/x", second);
    EXPECT_EQ(spec_of(reg)["paths"]["/x"]["get"]["summary"], "second");
}
