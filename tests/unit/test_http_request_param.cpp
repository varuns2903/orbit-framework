#include <gtest/gtest.h>
#include <orbit/http/HttpRequest.hpp>

// HttpRequest::param<T>() (#201): parses params as T, or std::nullopt.

using orbit::http::HttpRequest;

TEST(HttpRequestParamTest, MissingNameIsNullopt) {
    HttpRequest req;
    EXPECT_EQ(req.param<int>("id"), std::nullopt);
}

TEST(HttpRequestParamTest, IntParsesACleanInteger) {
    HttpRequest req;
    req.params["id"] = "42";
    EXPECT_EQ(req.param<int>("id"), 42);
    req.params["id"] = "-7";
    EXPECT_EQ(req.param<int>("id"), -7);
}

TEST(HttpRequestParamTest, IntRejectsTrailingJunkAndNonDigits) {
    HttpRequest req;
    req.params["id"] = "42abc";
    EXPECT_EQ(req.param<int>("id"), std::nullopt);
    req.params["id"] = "4.2";
    EXPECT_EQ(req.param<int>("id"), std::nullopt);
    req.params["id"] = "";
    EXPECT_EQ(req.param<int>("id"), std::nullopt);
    req.params["id"] = " 42";
    EXPECT_EQ(req.param<int>("id"), std::nullopt);
}

TEST(HttpRequestParamTest, DoubleParsesDecimals) {
    HttpRequest req;
    req.params["price"] = "19.99";
    auto price = req.param<double>("price");
    ASSERT_TRUE(price.has_value());
    EXPECT_DOUBLE_EQ(*price, 19.99);
}

TEST(HttpRequestParamTest, BoolAcceptsTrueFalseAndOneZero) {
    HttpRequest req;
    req.params["flag"] = "true";
    EXPECT_EQ(req.param<bool>("flag"), true);
    req.params["flag"] = "false";
    EXPECT_EQ(req.param<bool>("flag"), false);
    req.params["flag"] = "1";
    EXPECT_EQ(req.param<bool>("flag"), true);
    req.params["flag"] = "0";
    EXPECT_EQ(req.param<bool>("flag"), false);
    req.params["flag"] = "yes";
    EXPECT_EQ(req.param<bool>("flag"), std::nullopt);
}

TEST(HttpRequestParamTest, StringPassesThroughWithoutValidation) {
    HttpRequest req;
    req.params["slug"] = "hello-world";
    EXPECT_EQ(req.param<std::string>("slug"), "hello-world");
}

// A value a {id:int} route pattern already matched always parses as int.
TEST(HttpRequestParamTest, ATypeMatchedByTheRoutePatternAlwaysParses) {
    HttpRequest req;
    for (const char* value : {"0", "42", "-1", "999999999"}) {
        req.params["id"] = value;
        EXPECT_TRUE(req.param<int64_t>("id").has_value()) << value;
    }
}
