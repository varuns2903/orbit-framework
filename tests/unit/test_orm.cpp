#include <gtest/gtest.h>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/QueryBuilder.hpp>

using namespace orm;

TEST(OrmTest, ExpressionBuilder) {
    auto q = Col("is_active") == true;
    EXPECT_EQ(q.sql, "is_active = 'true'");
    
    auto q2 = Col("age") > 18;
    EXPECT_EQ(q2.sql, "age > '18'");

    auto q3 = Col("name") == "John" && Col("age") >= 21;
    EXPECT_EQ(q3.sql, "(name = 'John' AND age >= '21')");
}

TEST(OrmTest, ExpressionOr) {
    auto q = Col("status") == "active" || Col("role") == "admin";
    EXPECT_EQ(q.sql, "(status = 'active' OR role = 'admin')");
}

TEST(OrmTest, ExpressionLessThan) {
    auto q = Col("price") < 100;
    EXPECT_EQ(q.sql, "price < '100'");
}

TEST(OrmTest, ExpressionLessThanOrEqual) {
    auto q = Col("qty") <= 0;
    EXPECT_EQ(q.sql, "qty <= '0'");
}

TEST(OrmTest, ExpressionNotEqual) {
    auto q = Col("deleted") != true;
    EXPECT_EQ(q.sql, "deleted != 'true'");
}

TEST(OrmTest, ComplexNestedExpression) {
    auto q = (Col("age") >= 18 && Col("is_active") == true) || Col("role") == "admin";
    EXPECT_FALSE(q.sql.empty());
}


