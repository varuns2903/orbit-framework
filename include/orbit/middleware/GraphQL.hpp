#pragma once
#include <orbit/http/HttpRequest.hpp>
#include <orbit/http/HttpResponse.hpp>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace middleware {

/**
 * @brief Signature for a GraphQL executor function.
 * Takes the query string, operation name (optional), and variables (optional),
 * and returns a JSON response (which should have "data" or "errors").
 */
using GraphQLExecutor = std::function<nlohmann::json(const std::string& query, const std::string& operation_name, const nlohmann::json& variables)>;

/**
 * @brief Creates a middleware handler for GraphQL endpoints.
 * It parses the incoming GET or POST request according to the GraphQL over HTTP spec,
 * extracts the query, operationName, and variables, and invokes the provided executor.
 * 
 * @param executor The user-provided GraphQL execution engine function.
 */
inline std::function<bool(http::HttpRequest&, std::shared_ptr<http::ResponseWriter>)> graphql(GraphQLExecutor executor) {
    return [executor](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> res) -> bool {
        std::string query;
        std::string operation_name;
        nlohmann::json variables = nlohmann::json::object();

        if (req.method == http::HttpMethod::POST) {
            if (req.headers.count("Content-Type") && req.headers.at("Content-Type").find("application/json") != std::string::npos) {
                auto body = req.json();
                if (body.contains("query") && body["query"].is_string()) {
                    query = body["query"].get<std::string>();
                }
                if (body.contains("operationName") && body["operationName"].is_string()) {
                    operation_name = body["operationName"].get<std::string>();
                }
                if (body.contains("variables") && body["variables"].is_object()) {
                    variables = body["variables"];
                }
            } else if (req.headers.count("Content-Type") && req.headers.at("Content-Type").find("application/graphql") != std::string::npos) {
                query = req.body;
            }
        } else if (req.method == http::HttpMethod::GET) {
            // Parse from URL parameters
            auto it_query = req.query.find("query");
            if (it_query != req.query.end()) query = it_query->second;

            auto it_op = req.query.find("operationName");
            if (it_op != req.query.end()) operation_name = it_op->second;

            auto it_vars = req.query.find("variables");
            if (it_vars != req.query.end()) {
                try {
                    variables = nlohmann::json::parse(it_vars->second);
                } catch (...) {
                    // Ignore or handle invalid JSON variables in GET
                }
            }
        }

        if (query.empty()) {
            http::HttpResponse response;
            response.status_code = http::HttpStatus::BadRequest;
            response.json(std::string("{\"errors\": [{\"message\": \"GraphQL query is missing\"}]}"));
            res->send(std::move(response));
            return false;
        }

        try {
            nlohmann::json result = executor(query, operation_name, variables);
            http::HttpResponse response;
            response.status_code = http::HttpStatus::OK;
            response.json(std::string(result.dump()));
            res->send(std::move(response));
        } catch (const std::exception& e) {
            http::HttpResponse response;
            response.status_code = http::HttpStatus::InternalServerError;
            response.json(std::string("{\"errors\": [{\"message\": \"") + e.what() + "\"}]}");
            res->send(std::move(response));
        }

        return false; // Stop middleware chain, response sent
    };
}

} // namespace middleware
