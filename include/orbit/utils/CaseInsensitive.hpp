#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <string_view>
#include <cctype>
#include <algorithm>

namespace orbit::utils {

struct CaseInsensitiveHash {
    template <typename StringType>
    size_t operator()(const StringType& key) const {
        size_t hash = 0;
        for (char c : key) {
            hash ^= std::hash<char>{}(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

struct CaseInsensitiveEqual {
    template <typename StringType>
    bool operator()(const StringType& lhs, const StringType& rhs) const {
        if (lhs.size() != rhs.size()) return false;
        for (size_t i = 0; i < lhs.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
                return false;
            }
        }
        return true;
    }
};

} // namespace utils
