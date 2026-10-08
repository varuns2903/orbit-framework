#include <gtest/gtest.h>
#include <orbit/database/PostgresCoro.hpp>
#include <orbit/orm/QueryBuilder.hpp>

using namespace orbit::orm;

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

namespace {

struct Item {
    std::string name;
    int qty = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Item, name, qty)

// Stands in for MysqlClient: escaping instead of binding marks the dialect.
struct FakeMysql {
    struct Awaiter {
        bool await_ready() const { return true; }
        void await_suspend(std::coroutine_handle<>) {}
        orbit::database::ResultSet await_resume() { return {}; }
    };
    std::string escape(const std::string& s) { return s; }
    Awaiter query_async(const std::string&) { return {}; }
};

using PgItems = QueryBuilder<orbit::database::PostgresClient, Item>;
using MyItems = QueryBuilder<FakeMysql, Item>;

} // namespace

TEST(OrmBuilderTest, SelectWithOrderLimitOffset) {
    PgItems q(nullptr, "items");
    q.where(Col("qty") > 3).order_by("name").order_by("qty", Order::Desc).limit(10).offset(20);
    Statement st = q.select_statement();
    EXPECT_EQ(st.sql, "SELECT * FROM items WHERE qty > ? ORDER BY name ASC, qty DESC LIMIT 10 OFFSET 20");
    EXPECT_EQ(st.params, P({"3"}));
}

TEST(OrmBuilderTest, MysqlOffsetNeedsALimit) {
    MyItems q(nullptr, "items");
    q.offset(5);
    EXPECT_EQ(q.select_statement().sql, "SELECT * FROM items LIMIT 18446744073709551615 OFFSET 5");
    PgItems pg(nullptr, "items");
    pg.offset(5);
    EXPECT_EQ(pg.select_statement().sql, "SELECT * FROM items OFFSET 5");
}

TEST(OrmBuilderTest, ReservedWordsAreQuotedPerDialect) {
    PgItems pg(nullptr, "user");
    pg.where(Col("order") == 1).where("group", "=", "a").order_by("desc");
    EXPECT_EQ(pg.select_statement().sql,
              "SELECT * FROM \"user\" WHERE \"order\" = ? AND \"group\" = ? ORDER BY \"desc\" ASC");

    MyItems my(nullptr, "user");
    my.where(Col("t.order") == 1);
    EXPECT_EQ(my.select_statement().sql, "SELECT * FROM `user` WHERE t.`order` = ?");

    // Ordinary names stay unquoted, so PostgreSQL's case folding is unchanged.
    PgItems plain(nullptr, "CreatedItems");
    plain.where(Col("createdAt") > 0);
    EXPECT_EQ(plain.select_statement().sql, "SELECT * FROM CreatedItems WHERE createdAt > ?");
}

TEST(OrmBuilderTest, UpdateBindsEveryValue) {
    PgItems q(nullptr, "items");
    q.where(Col("name") == "widget");
    Statement st = q.update_statement({{"qty", 7}, {"order", nullptr}});
    EXPECT_EQ(st.sql, "UPDATE items SET \"order\" = ?, qty = ? WHERE name = ?");
    ASSERT_EQ(st.params.size(), 3u);
    EXPECT_EQ(st.params[0], std::nullopt);
    EXPECT_EQ(st.params[1], "7");
    EXPECT_EQ(st.params[2], "widget");
}

TEST(OrmBuilderTest, UpdateAndDeleteRefuseToTouchEveryRowByAccident) {
    PgItems q(nullptr, "items");
    EXPECT_THROW(q.update_statement({{"qty", 0}}), std::logic_error);
    EXPECT_THROW(q.delete_statement(), std::logic_error);
    q.all();
    EXPECT_EQ(q.update_statement({{"qty", 0}}).sql, "UPDATE items SET qty = ?");
    EXPECT_EQ(q.delete_statement().sql, "DELETE FROM items");
}

TEST(OrmBuilderTest, UpdateRejectsBadInput) {
    PgItems q(nullptr, "items");
    q.where(Col("qty") == 1);
    EXPECT_THROW(q.update_statement(nlohmann::json::object()), std::invalid_argument);
    EXPECT_THROW(q.update_statement(nlohmann::json::array({1})), std::invalid_argument);
    EXPECT_THROW(q.update_statement({{"qty = 0; DROP TABLE items; --", 1}}), std::invalid_argument);
    EXPECT_THROW(q.order_by("name; DROP TABLE items"), std::invalid_argument);
}

TEST(OrmBuilderTest, DeleteAndCount) {
    PgItems q(nullptr, "items");
    q.where(Col("qty") <= 0 || Col("name") == "old");
    EXPECT_EQ(q.delete_statement().sql, "DELETE FROM items WHERE (qty <= ? OR name = ?)");
    EXPECT_EQ(q.count_statement().sql, "SELECT COUNT(*) AS n FROM items WHERE (qty <= ? OR name = ?)");
    EXPECT_EQ(q.count_statement().params, P({"0", "old"}));
}
