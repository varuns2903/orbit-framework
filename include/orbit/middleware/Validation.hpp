#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <orbit/routing/Router.hpp>
#include <vector>
#include <string>

namespace orbit::middleware {

/**
 * @ingroup middlewares
 * @brief Enumeration of JSON data types.
 */
enum class JsonType {
    STRING,
    NUMBER,
    BOOLEAN,
    OBJECT,
    ARRAY
};

/**
 * @ingroup middlewares
 * @brief Represents a field in a validation schema.
 */
struct SchemaField {
    std::string name;
    JsonType type;
    bool required = true;
};

/**
 * @ingroup middlewares
 * @brief Returns a middleware that parses and validates a JSON request body.
 * 
 * If valid, the parsed JSON is available in req.json_body.
 * If invalid, it intercepts the request and responds with a 422 Unprocessable Entity.
 * 
 * @param schema The schema to validate against.
 * @return routing::Middleware The validation middleware handler.
 */
routing::Middleware validate_json(const std::vector<SchemaField>& schema);

} // namespace middleware
