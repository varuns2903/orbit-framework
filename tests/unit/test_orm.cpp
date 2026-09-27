#include <gtest/gtest.h>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/QueryBuilder.hpp>

using namespace orm;

namespace {
Params P(std::initializer_list<const char*> values) {
    Params out;
    for (const char* v : values) out.emplace_back(v);
    return out;
}
} // namespace

TEST(OrmTest, ExpressionBuilder) {
    auto q = Col("is_active") == true;
    EXPECT_EQ(q.sql, "is_active = ?");
    EXPECT_EQ(q.params, P({"1"}));
    
    auto q2 = Col("age") > 18;
    EXPECT_EQ(q2.sql, "age > ?");
    EXPECT_EQ(q2.params, P({"18"}));

    auto q3 = Col("name") == "John" && Col("age") >= 21;
    EXPECT_EQ(q3.sql, "(name = ? AND age >= ?)");
    EXPECT_EQ(q3.params, P({"John", "21"}));
}

TEST(OrmTest, ExpressionOr) {
    auto q = Col("status") == "active" || Col("role") == "admin";
    EXPECT_EQ(q.sql, "(status = ? OR role = ?)");
    EXPECT_EQ(q.params, P({"active", "admin"}));
}

TEST(OrmTest, ExpressionLessThan) {
    auto q = Col("price") < 100;
    EXPECT_EQ(q.sql, "price < ?");
}

TEST(OrmTest, ExpressionLessThanOrEqual) {
    auto q = Col("qty") <= 0;
    EXPECT_EQ(q.sql, "qty <= ?");
}

TEST(OrmTest, ExpressionNotEqual) {
    auto q = Col("deleted") != true;
    EXPECT_EQ(q.sql, "deleted != ?");
}

TEST(OrmTest, ComplexNestedExpression) {
    auto q = (Col("age") >= 18 && Col("is_active") == true) || Col("role") == "admin";
    EXPECT_EQ(q.sql, "((age >= ? AND is_active = ?) OR role = ?)");
    EXPECT_EQ(q.params, P({"18", "1", "admin"}));
}

TEST(OrmTest, ValuesNeverReachTheSqlText) {
    const char* evil = "x' OR '1'='1'; DROP TABLE users; --";
    auto q = Col("name") == evil;
    EXPECT_EQ(q.sql, "name = ?");
    EXPECT_EQ(q.params, P({evil}));
}

TEST(OrmTest, RejectsNonIdentifierColumns) {
    EXPECT_THROW(Col("name; DROP TABLE users"), std::invalid_argument);
    EXPECT_THROW(Col("1abc"), std::invalid_argument);
    EXPECT_THROW(Col(""), std::invalid_argument);
    EXPECT_THROW(Col("a..b"), std::invalid_argument);
    EXPECT_NO_THROW(Col("users.created_at"));
}

TEST(OrmTest, PlaceholderRendering) {
    EXPECT_EQ(detail::number_placeholders("a = ? AND b = ?"), "a = $1 AND b = $2");
    Params params = {std::string("O'Brien"), std::nullopt};
    auto escape = [](const std::string& v) {
        std::string out;
        for (char c : v) { if (c == '\'') out += "\\'"; else out += c; }
        return out;
    };
    EXPECT_EQ(detail::inline_literals("a = ? AND b = ?", params, escape), "a = 'O\\'Brien' AND b = NULL");
}

TEST(OrmTest, FloatingPointKeepsPrecision) {
    auto q = Col("ratio") == 0.1;
    ASSERT_TRUE(q.params[0].has_value());
    EXPECT_EQ(std::stod(*q.params[0]), 0.1);
}
