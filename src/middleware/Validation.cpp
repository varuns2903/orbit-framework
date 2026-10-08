#include <orbit/middleware/Validation.hpp>
#include <iostream>

namespace orbit::middleware {

routing::Middleware validate_json(const std::vector<SchemaField>& schema) {
    return [schema](http::HttpRequest& request, std::shared_ptr<http::ResponseWriter> writer) -> bool {
        // Parse the body
        nlohmann::json j = request.json();
        if (j.is_discarded() || !j.is_object()) {
            http::HttpResponse res;
            res.status(http::HttpStatus::UnprocessableEntity).send(R"({"error": "Invalid JSON payload"})");
            res.headers["Content-Type"] = "application/json";
            writer->send(std::move(res));
            return false;
        }

        nlohmann::json errors = nlohmann::json::array();

        for (const auto& field : schema) {
            auto it = j.find(field.name);
            if (it == j.end()) {
                if (field.required) {
                    errors.push_back("Missing required field: " + field.name);
                }
                continue;
            }

            bool type_valid = false;
            switch (field.type) {
                case JsonType::STRING: type_valid = it->is_string(); break;
                case JsonType::NUMBER: type_valid = it->is_number(); break;
                case JsonType::BOOLEAN: type_valid = it->is_boolean(); break;
                case JsonType::OBJECT: type_valid = it->is_object(); break;
                case JsonType::ARRAY: type_valid = it->is_array(); break;
            }

            if (!type_valid) {
                errors.push_back("Invalid type for field '" + field.name + "'");
            }
        }

        if (!errors.empty()) {
            http::HttpResponse res;
            nlohmann::json response_body;
            response_body["error"] = "Validation failed";
            response_body["details"] = errors;
            
            res.status(http::HttpStatus::UnprocessableEntity).send(response_body.dump());
            res.headers["Content-Type"] = "application/json";
            writer->send(std::move(res));
            return false;
        }

        return true;
    };
}

} // namespace middleware
