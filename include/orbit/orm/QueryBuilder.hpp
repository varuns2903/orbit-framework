#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <coroutine>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <limits>
#include <orbit/http/json.hpp>
#include <orbit/database/ResultSet.hpp>

namespace orm {

/**
 * @brief Values bound to the `?` placeholders of an expression, in order.
 *        std::nullopt binds SQL NULL.
 */
using Params = std::vector<std::optional<std::string>>;

namespace detail {

/**
 * @brief True for a plain SQL identifier (`name`) or a qualified one
 *        (`table.name`): ASCII letters, digits and underscores, not starting
 *        with a digit. Identifiers cannot be bound as parameters, so anything
 *        else is rejected rather than spliced into SQL.
 */
inline bool is_identifier(std::string_view s) {
    if (s.empty()) return false;
    bool part_start = true;
    for (char c : s) {
        if (c == '.') {
            if (part_start) return false;
            part_start = true;
            continue;
        }
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        bool digit = (c >= '0' && c <= '9');
        if (!(alpha || (digit && !part_start))) return false;
        part_start = false;
    }
    return !part_start;
}

inline const std::string& checked_identifier(const std::string& s) {
    if (!is_identifier(s)) throw std::invalid_argument("Invalid SQL identifier: " + s);
    return s;
}

inline const std::string& checked_operator(const std::string& op) {
    static const char* const allowed[] = {"=", "!=", "<>", "<", "<=", ">", ">=", "LIKE", "NOT LIKE", "ILIKE", "NOT ILIKE"};
    for (const char* a : allowed) {
        if (op == a) return op;
    }
    throw std::invalid_argument("Unsupported SQL comparison operator: " + op);
}

template <typename T>
std::optional<std::string> to_param(const T& val) {
    if constexpr (std::is_same_v<T, bool>) {
        return std::string(val ? "1" : "0"); // valid for PostgreSQL booleans and MySQL TINYINT(1)
    } else if constexpr (std::is_integral_v<T>) {
        return std::to_string(val);
    } else if constexpr (std::is_floating_point_v<T>) {
        std::ostringstream os;
        os.precision(std::numeric_limits<T>::max_digits10);
        os << val;
        return os.str();
    } else {
        return std::string(val);
    }
}

inline std::optional<std::string> json_to_param(const nlohmann::json& v) {
    if (v.is_null()) return std::nullopt;
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return std::string(v.get<bool>() ? "1" : "0");
    return v.dump(); // numbers, and objects/arrays as JSON text
}

/// Rewrites `?` placeholders as PostgreSQL's `$1`, `$2`, ...
inline std::string number_placeholders(const std::string& sql) {
    std::string out;
    out.reserve(sql.size() + 8);
    int n = 0;
    for (char c : sql) {
        if (c == '?') out += "$" + std::to_string(++n);
        else out.push_back(c);
    }
    return out;
}

/// Replaces `?` placeholders with escaped, quoted literals (for clients without parameter binding).
template <typename Escape>
std::string inline_literals(const std::string& sql, const Params& params, Escape&& escape) {
    std::string out;
    size_t i = 0;
    for (char c : sql) {
        if (c != '?') {
            out.push_back(c);
            continue;
        }
        if (i >= params.size()) throw std::logic_error("Fewer parameters than placeholders");
        const auto& p = params[i++];
        out += p ? "'" + escape(*p) + "'" : std::string("NULL");
    }
    return out;
}

} // namespace detail

/**
 * @brief A SQL condition with `?` placeholders and the values bound to them.
 */
struct Expr {
    std::string sql;
    Params params;

    Expr operator&&(const Expr& other) const {
        Params combined = params;
        combined.insert(combined.end(), other.params.begin(), other.params.end());
        return Expr{"(" + sql + " AND " + other.sql + ")", std::move(combined)};
    }

    Expr operator||(const Expr& other) const {
        Params combined = params;
        combined.insert(combined.end(), other.params.begin(), other.params.end());
        return Expr{"(" + sql + " OR " + other.sql + ")", std::move(combined)};
    }
};

/**
 * @brief A database column, allowing C++ operators to build parameterized SQL conditions.
 *
 * Values are always bound as parameters, never spliced into the SQL text.
 * @throws std::invalid_argument if the column name is not a plain identifier.
 */
class Col {
public:
    explicit Col(std::string name) : name_(detail::checked_identifier(name)) {}

    template <typename T>
    Expr operator==(const T& val) const { return make("=", val); }

    template <typename T>
    Expr operator!=(const T& val) const { return make("!=", val); }

    template <typename T>
    Expr operator>(const T& val) const { return make(">", val); }

    template <typename T>
    Expr operator<(const T& val) const { return make("<", val); }

    template <typename T>
    Expr operator>=(const T& val) const { return make(">=", val); }

    template <typename T>
    Expr operator<=(const T& val) const { return make("<=", val); }

private:
    std::string name_;

    template <typename T>
    Expr make(const char* op, const T& val) const {
        return Expr{name_ + " " + op + " ?", {detail::to_param(val)}};
    }
};

/**
 * @brief Runs @p sql (with `?` placeholders) on either client type.
 *
 * PostgreSQL binds the values as real parameters. MariaDB/MySQL clients, whose
 * non-blocking API has no parameter binding here, receive literals escaped with
 * mysql_real_escape_string.
 */
template <typename DBClient>
auto do_query_async(std::shared_ptr<DBClient> client, const std::string& sql, const Params& params) {
    if constexpr (requires { client->escape(std::string{}); }) {
        return client->query_async(detail::inline_literals(sql, params, [&](const std::string& v) { return client->escape(v); }));
    } else {
        return database::query_async(client, detail::number_placeholders(sql), params);
    }
}

template <typename DBClient, typename ModelType>
struct QueryGetAwaiter {
    using NativeAwaiter = decltype(do_query_async(std::declval<std::shared_ptr<DBClient>>(), std::declval<std::string>(), std::declval<Params>()));
    NativeAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }

    std::vector<ModelType> await_resume() {
        database::ResultSet rs = native_awaiter.await_resume();
        return rs.to_json().get<std::vector<ModelType>>();
    }
};

template <typename DBClient, typename ModelType>
struct QueryInsertAwaiter {
    using NativeAwaiter = decltype(do_query_async(std::declval<std::shared_ptr<DBClient>>(), std::declval<std::string>(), std::declval<Params>()));
    NativeAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }

    uint64_t await_resume() {
        database::ResultSet rs = native_awaiter.await_resume();
        return rs.affected_rows();
    }
};

/**
 * @brief A lightweight, JSON-backed ORM Query Builder with bound parameters.
 */
template <typename DBClient, typename ModelType>
class QueryBuilder {
public:
    /// @throws std::invalid_argument if @p table is not a plain identifier.
    QueryBuilder(std::shared_ptr<DBClient> db, const std::string& table)
        : db_(std::move(db)), table_(detail::checked_identifier(table)) {}

    /**
     * @brief Adds a WHERE condition. @p val is bound as a parameter.
     * @throws std::invalid_argument for an invalid field name or an operator
     *         other than =, !=, <>, <, <=, >, >=, [NOT] LIKE, [NOT] ILIKE.
     */
    QueryBuilder& where(const std::string& field, const std::string& op, const std::string& val) {
        wheres_.push_back(Expr{detail::checked_identifier(field) + " " + detail::checked_operator(op) + " ?", {val}});
        return *this;
    }

    /**
     * @brief Adds a WHERE clause using C++ Expression Templates for a clean DSL.
     *
     * @example query_User(db).where(orm::Col("age") >= 18 && orm::Col("is_active") == true)
     */
    QueryBuilder& where(const Expr& expr) {
        wheres_.push_back(expr);
        return *this;
    }

    /**
     * @brief Executes a SELECT query asynchronously and maps results to a vector of ModelType.
     */
    QueryGetAwaiter<DBClient, ModelType> get_async() {
        std::string sql = "SELECT * FROM " + table_;
        Params params;
        if (!wheres_.empty()) {
            sql += " WHERE ";
            for (size_t i = 0; i < wheres_.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += wheres_[i].sql;
                params.insert(params.end(), wheres_[i].params.begin(), wheres_[i].params.end());
            }
        }
        return QueryGetAwaiter<DBClient, ModelType>{ do_query_async(db_, sql, params) };
    }

    /**
     * @brief Executes an INSERT query for a model, binding every value.
     * @throws std::invalid_argument if a field name is not a plain identifier.
     */
    QueryInsertAwaiter<DBClient, ModelType> insert_async(const ModelType& model) {
        nlohmann::json j = model;
        std::string cols;
        std::string vals;
        Params params;

        bool first = true;
        for (auto& el : j.items()) {
            if (!first) { cols += ", "; vals += ", "; }
            cols += detail::checked_identifier(el.key());
            vals += "?";
            params.push_back(detail::json_to_param(el.value()));
            first = false;
        }

        std::string sql = "INSERT INTO " + table_ + " (" + cols + ") VALUES (" + vals + ");";
        return QueryInsertAwaiter<DBClient, ModelType>{ do_query_async(db_, sql, params) };
    }

private:
    std::shared_ptr<DBClient> db_;
    std::string table_;
    std::vector<Expr> wheres_;
};

} // namespace orm
