#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <stdexcept>
#include <charconv>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <orbit/http/json.hpp>

namespace database {

namespace detail {

/**
 * @brief Converts a column's text to T: bool, an integer, a floating-point
 *        type or std::string. The whole text must convert; "12abc" is not 12.
 *        Booleans accept t/f, true/false, 1/0 (PostgreSQL and MySQL spellings).
 */
template <typename T>
std::optional<T> convert(const std::string& text) {
    if constexpr (std::is_same_v<T, std::string>) {
        return text;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "t" || text == "true" || text == "1" || text == "TRUE") return true;
        if (text == "f" || text == "false" || text == "0" || text == "FALSE") return false;
        return std::nullopt;
    } else if constexpr (std::is_integral_v<T>) {
        T value{};
        auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
        return value;
    } else if constexpr (std::is_floating_point_v<T>) {
        // A stream with the classic locale: strtod depends on the process locale.
        std::istringstream in(text);
        in.imbue(std::locale::classic());
        T value{};
        in >> value;
        if (in.fail() || in.peek() != std::char_traits<char>::eof()) {
            if (text == "NaN") return std::numeric_limits<T>::quiet_NaN();
            if (text == "Infinity") return std::numeric_limits<T>::infinity();
            if (text == "-Infinity") return -std::numeric_limits<T>::infinity();
            return std::nullopt;
        }
        return value;
    } else {
        static_assert(sizeof(T) == 0, "get_as<T> supports bool, integers, floating point and std::string");
    }
}

} // namespace detail

/**
 * @brief Represents a single row of a database result set.
 */
class Row {
public:
    Row() = default;
    Row(std::vector<std::string> values, std::shared_ptr<std::unordered_map<std::string, size_t>> col_map)
        : values_(values.begin(), values.end()), col_map_(std::move(col_map)) {}

    /// Values may be std::nullopt for SQL NULL.
    Row(std::vector<std::optional<std::string>> values, std::shared_ptr<std::unordered_map<std::string, size_t>> col_map)
        : values_(std::move(values)), col_map_(std::move(col_map)) {}

    /**
     * @brief Gets a column value by index.
     * @param index The zero-based column index.
     * @return An optional containing the string value, or std::nullopt if index is out of bounds or value is NULL.
     */
    std::optional<std::string> get(size_t index) const {
        if (index < values_.size()) {
            return values_[index];
        }
        return std::nullopt;
    }

    /**
     * @brief Gets a column value by column name.
     * @param col_name The name of the column.
     * @return An optional containing the string value, or std::nullopt if not found or NULL.
     */
    std::optional<std::string> get(const std::string& col_name) const {
        if (!col_map_) return std::nullopt;
        auto it = col_map_->find(col_name);
        if (it != col_map_->end()) {
            return get(it->second);
        }
        return std::nullopt;
    }

    /**
     * @brief Gets a column converted to T (bool, an integer type, a floating
     *        point type or std::string).
     * @return std::nullopt if the column is missing, NULL, or does not
     *         convert exactly (out of range, trailing characters).
     */
    template <typename T>
    std::optional<T> get_as(size_t index) const {
        auto text = get(index);
        if (!text) return std::nullopt;
        return detail::convert<T>(*text);
    }

    template <typename T>
    std::optional<T> get_as(const std::string& col_name) const {
        auto text = get(col_name);
        if (!text) return std::nullopt;
        return detail::convert<T>(*text);
    }

    /// get_as<T>(), or @p fallback when that is std::nullopt.
    template <typename T>
    T value_or(const std::string& col_name, T fallback) const {
        return get_as<T>(col_name).value_or(std::move(fallback));
    }

    /// True if the column exists and is SQL NULL.
    bool is_null(const std::string& col_name) const {
        if (!col_map_) return false;
        auto it = col_map_->find(col_name);
        return it != col_map_->end() && it->second < values_.size() && !values_[it->second];
    }

    /**
     * @brief Helper to get a column value as an integer.
     */
    int as_int(const std::string& col_name, int default_val = 0) const {
        auto val = get(col_name);
        if (!val || val->empty()) return default_val;
        try { return std::stoi(*val); } catch (...) { return default_val; }
    }

    /**
     * @brief Serializes the row to a JSON object automatically.
     * Tries to parse numbers and booleans from strings if possible (Task 52).
     */
    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::object();
        if (!col_map_) return j;
        
        for (const auto& [name, index] : *col_map_) {
            if (!values_[index]) {
                j[name] = nullptr; // SQL NULL
                continue;
            }
            const std::string& val = *values_[index];
            // Automatic JSON serialization heuristics (Task 52)
            if (val.empty()) {
                j[name] = "";
                continue;
            }
            
            if (val == "true" || val == "t" || val == "TRUE") {
                j[name] = true;
            } else if (val == "false" || val == "f" || val == "FALSE") {
                j[name] = false;
            } else {
                // Try to parse as integer or double, otherwise keep as string
                try {
                    size_t pos = 0;
                    long long int_val = std::stoll(val, &pos);
                    if (pos == val.length()) { // Entire string was parsed as int
                        j[name] = int_val;
                        continue;
                    }
                } catch (...) {}
                
                try {
                    size_t pos = 0;
                    double dbl_val = std::stod(val, &pos);
                    if (pos == val.length()) { // Entire string was parsed as double
                        j[name] = dbl_val;
                        continue;
                    }
                } catch (...) {}
                
                j[name] = val;
            }
        }
        return j;
    }

private:
    std::vector<std::optional<std::string>> values_;
    std::shared_ptr<std::unordered_map<std::string, size_t>> col_map_;
};

/**
 * @brief Represents a generic, unified result set from a database query.
 */
class ResultSet {
public:
    ResultSet() = default;
    ResultSet(std::vector<Row> rows, uint64_t affected_rows = 0) 
        : rows_(std::move(rows)), affected_rows_(affected_rows) {}

    /// A result describing a failed query.
    static ResultSet failure(std::string message) {
        ResultSet r;
        r.error_ = std::move(message);
        if (r.error_.empty()) r.error_ = "unknown database error";
        return r;
    }

    /// False if the query failed; error() then says why.
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    const std::vector<Row>& rows() const { return rows_; }
    size_t size() const { return rows_.size(); }
    bool empty() const { return rows_.empty(); }
    uint64_t affected_rows() const { return affected_rows_; }

    const Row& operator[](size_t index) const {
        if (index >= rows_.size()) {
            throw std::out_of_range("ResultSet index out of range");
        }
        return rows_[index];
    }

    /**
     * @brief Serializes the entire result set to a JSON array of objects.
     */
    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& row : rows_) {
            j.push_back(row.to_json());
        }
        return j;
    }

private:
    std::vector<Row> rows_;
    uint64_t affected_rows_{0};
    std::string error_;
};

} // namespace database
