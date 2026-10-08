#pragma once
#include <orbit/legacy_namespaces.hpp>
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
#include <charconv>
#include <orbit/http/json.hpp>
#include <orbit/database/ResultSet.hpp>

namespace orbit::orm {

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

/// SQL reserved words likely to be used as table or column names. These are
/// quoted; other identifiers stay unquoted, so PostgreSQL still folds them to
/// lower case exactly as when the table was created without quotes.
inline bool is_reserved_word(std::string_view s) {
    static const char* const words[] = {
        "all", "and", "any", "as", "asc", "between", "both", "by", "case", "check", "column", "constraint",
        "create", "cross", "current_date", "current_time", "current_timestamp", "current_user", "default",
        "delete", "desc", "distinct", "do", "else", "end", "except", "exists", "false", "fetch", "for",
        "foreign", "from", "full", "grant", "group", "having", "in", "index", "inner", "insert", "intersect",
        "into", "is", "join", "key", "leading", "left", "like", "limit", "natural", "not", "null", "offset",
        "on", "only", "or", "order", "outer", "primary", "references", "returning", "right", "select",
        "session_user", "set", "some", "table", "then", "to", "trailing", "true", "union", "unique", "update",
        "user", "using", "values", "when", "where", "window", "with"};
    std::string lower(s);
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char* w : words) {
        if (lower == w) return true;
    }
    return false;
}

/// Marks a reserved word in generated SQL; replaced by the dialect's quote
/// when the statement is sent (render_identifiers). Values are always bound
/// separately, so this character never comes from user data.
inline constexpr char kQuoteMark = '\x01';

/**
 * @brief Validates an identifier (`name` or `table.name`) and marks the parts
 *        that are reserved words for quoting.
 * @throws std::invalid_argument if it is not a plain identifier.
 */
inline std::string ident(const std::string& name) {
    checked_identifier(name);
    std::string out;
    size_t start = 0;
    while (true) {
        size_t dot = name.find('.', start);
        std::string part = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (is_reserved_word(part)) out += kQuoteMark + part + kQuoteMark;
        else out += part;
        if (dot == std::string::npos) break;
        out += '.';
        start = dot + 1;
    }
    return out;
}

/// Replaces quote marks with `"` (PostgreSQL, standard SQL) or a backtick (MySQL).
inline std::string render_identifiers(std::string sql, bool mysql) {
    for (char& c : sql) {
        if (c == kQuoteMark) c = mysql ? '`' : '"';
    }
    return sql;
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
    explicit Col(std::string name) : name_(detail::ident(name)) {}

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
/// True for clients speaking MySQL/MariaDB (they escape rather than bind).
template <typename DBClient>
constexpr bool is_mysql_client() {
    return requires(std::shared_ptr<DBClient> c) { c->escape(std::string{}); };
}

template <typename DBClient>
auto do_query_async(std::shared_ptr<DBClient> client, const std::string& sql, const Params& params) {
    constexpr bool mysql = is_mysql_client<DBClient>();
    std::string rendered = detail::render_identifiers(sql, mysql);
    if constexpr (mysql) {
        return client->query_async(detail::inline_literals(rendered, params, [&](const std::string& v) { return client->escape(v); }));
    } else {
        return database::query_async(client, detail::number_placeholders(rendered), params);
    }
}

/// A statement with `?` placeholders and the values bound to them.
struct Statement {
    std::string sql;
    Params params;
};

/// Sort direction for QueryBuilder::order_by().
enum class Order { Asc, Desc };

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

template <typename DBClient>
struct QueryCountAwaiter {
    using NativeAwaiter = decltype(do_query_async(std::declval<std::shared_ptr<DBClient>>(), std::declval<std::string>(), std::declval<Params>()));
    NativeAwaiter native_awaiter;

    bool await_ready() const { return native_awaiter.await_ready(); }
    void await_suspend(std::coroutine_handle<> h) { native_awaiter.await_suspend(h); }

    uint64_t await_resume() {
        database::ResultSet rs = native_awaiter.await_resume();
        if (rs.size() == 0) return 0;
        std::string text = rs[0].get(0).value_or("0");
        uint64_t n = 0;
        std::from_chars(text.data(), text.data() + text.size(), n);
        return n;
    }
};

/**
 * @brief A lightweight, JSON-backed ORM Query Builder with bound parameters.
 *
 * Values are always bound, never spliced. Table and column names must be
 * plain identifiers; SQL reserved words among them (`user`, `order`, ...)
 * are quoted for the client's dialect.
 */
template <typename DBClient, typename ModelType>
class QueryBuilder {
public:
    /// @throws std::invalid_argument if @p table is not a plain identifier.
    QueryBuilder(std::shared_ptr<DBClient> db, const std::string& table)
        : db_(std::move(db)), table_(detail::ident(table)) {}

    /**
     * @brief Adds a WHERE condition. @p val is bound as a parameter.
     * @throws std::invalid_argument for an invalid field name or an operator
     *         other than =, !=, <>, <, <=, >, >=, [NOT] LIKE, [NOT] ILIKE.
     */
    QueryBuilder& where(const std::string& field, const std::string& op, const std::string& val) {
        wheres_.push_back(Expr{detail::ident(field) + " " + detail::checked_operator(op) + " ?", {val}});
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

    /// Sorts the results; call again for further sort keys.
    /// @throws std::invalid_argument if @p field is not a plain identifier.
    QueryBuilder& order_by(const std::string& field, Order direction = Order::Asc) {
        orders_.push_back(detail::ident(field) + (direction == Order::Desc ? " DESC" : " ASC"));
        return *this;
    }

    /// Returns at most @p n rows.
    QueryBuilder& limit(uint64_t n) {
        limit_ = n;
        return *this;
    }

    /// Skips the first @p n rows (use with order_by() for stable pages).
    QueryBuilder& offset(uint64_t n) {
        offset_ = n;
        return *this;
    }

    /// Allows update_async()/remove_async() without a WHERE condition, i.e.
    /// on every row. Without it they throw, so a forgotten where() cannot
    /// rewrite or empty a whole table.
    QueryBuilder& all() {
        all_rows_ = true;
        return *this;
    }

    /// The SELECT that get_async() runs (placeholders as `?`).
    Statement select_statement() const {
        Statement st{"SELECT * FROM " + table_, {}};
        append_where(st);
        if (!orders_.empty()) {
            st.sql += " ORDER BY ";
            for (size_t i = 0; i < orders_.size(); ++i) st.sql += (i ? ", " : "") + orders_[i];
        }
        // Integers, not parameters: MySQL rejects a quoted LIMIT value.
        if (limit_) {
            st.sql += " LIMIT " + std::to_string(*limit_);
        } else if (offset_ && is_mysql_client<DBClient>()) {
            st.sql += " LIMIT 18446744073709551615"; // MySQL has no OFFSET without LIMIT
        }
        if (offset_) st.sql += " OFFSET " + std::to_string(*offset_);
        return finish(st);
    }

    /// The SELECT COUNT(*) that count_async() runs.
    Statement count_statement() const {
        Statement st{"SELECT COUNT(*) AS n FROM " + table_, {}};
        append_where(st);
        return finish(st);
    }

    /**
     * @brief The UPDATE that update_async() runs.
     * @param changes A JSON object of column -> new value (null sets NULL).
     * @throws std::invalid_argument if @p changes is not a non-empty object
     *         or names an invalid column; std::logic_error without a WHERE
     *         condition unless all() was called.
     */
    Statement update_statement(const nlohmann::json& changes) const {
        if (!changes.is_object() || changes.empty()) {
            throw std::invalid_argument("update needs a JSON object with at least one column");
        }
        require_condition("update");
        Statement st{"UPDATE " + table_ + " SET ", {}};
        bool first = true;
        for (auto& el : changes.items()) {
            st.sql += (first ? "" : ", ") + detail::ident(el.key()) + " = ?";
            st.params.push_back(detail::json_to_param(el.value()));
            first = false;
        }
        append_where(st);
        return finish(st);
    }

    /// The DELETE that remove_async() runs.
    /// @throws std::logic_error without a WHERE condition unless all() was called.
    Statement delete_statement() const {
        require_condition("delete");
        Statement st{"DELETE FROM " + table_, {}};
        append_where(st);
        return finish(st);
    }

    /**
     * @brief Executes a SELECT query asynchronously and maps results to a vector of ModelType.
     */
    QueryGetAwaiter<DBClient, ModelType> get_async() {
        Statement st = select_statement();
        return QueryGetAwaiter<DBClient, ModelType>{ do_query_async(db_, st.sql, st.params) };
    }

    /// Counts the matching rows.
    QueryCountAwaiter<DBClient> count_async() {
        Statement st = count_statement();
        return QueryCountAwaiter<DBClient>{ do_query_async(db_, st.sql, st.params) };
    }

    /**
     * @brief Updates the matching rows; resumes with the number affected.
     * @code
     * co_await query_User(db).where(orm::Col("id") == 7).update_async({{"name", "Ada"}, {"active", true}});
     * @endcode
     */
    QueryInsertAwaiter<DBClient, ModelType> update_async(const nlohmann::json& changes) {
        Statement st = update_statement(changes);
        return QueryInsertAwaiter<DBClient, ModelType>{ do_query_async(db_, st.sql, st.params) };
    }

    /// Updates the matching rows to the fields of @p model.
    QueryInsertAwaiter<DBClient, ModelType> update_async(const ModelType& model) {
        return update_async(nlohmann::json(model));
    }

    /// Deletes the matching rows; resumes with the number affected.
    QueryInsertAwaiter<DBClient, ModelType> remove_async() {
        Statement st = delete_statement();
        return QueryInsertAwaiter<DBClient, ModelType>{ do_query_async(db_, st.sql, st.params) };
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
            cols += detail::ident(el.key());
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
    std::vector<std::string> orders_;
    std::optional<uint64_t> limit_;
    std::optional<uint64_t> offset_;
    bool all_rows_ = false;

    void append_where(Statement& st) const {
        if (wheres_.empty()) return;
        st.sql += " WHERE ";
        for (size_t i = 0; i < wheres_.size(); ++i) {
            if (i > 0) st.sql += " AND ";
            st.sql += wheres_[i].sql;
            st.params.insert(st.params.end(), wheres_[i].params.begin(), wheres_[i].params.end());
        }
    }

    void require_condition(const char* what) const {
        if (wheres_.empty() && !all_rows_) {
            throw std::logic_error(std::string(what) + " without a where() condition; call all() to affect every row");
        }
    }

    static Statement finish(Statement st) {
        st.sql = detail::render_identifiers(std::move(st.sql), is_mysql_client<DBClient>());
        return st;
    }
};

} // namespace orm
