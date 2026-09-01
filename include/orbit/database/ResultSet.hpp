#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace database {

/**
 * @brief Represents a single row of a database result set.
 */
class Row {
public:
    Row() = default;
    Row(std::vector<std::string> values, std::shared_ptr<std::unordered_map<std::string, size_t>> col_map)
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
            const std::string& val = values_[index];
            
            // Automatic JSON serialization heuristics (Task 52)
            if (val.empty()) {
                j[name] = nullptr;
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
    std::vector<std::string> values_;
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
};

} // namespace database
