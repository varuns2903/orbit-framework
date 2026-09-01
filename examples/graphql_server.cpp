#include <orbit/server/App.hpp>
#include <orbit/middleware/GraphQL.hpp>
#include <iostream>
#include <memory>

using namespace server;
using namespace http;
using namespace middleware;

int main(int argc, char* argv[]) {
    auto config = config::ServerConfig::parse(argc, argv);
    App app(config);

    // A mock executor that just echoes the query and variables back
    // In a real application, you would pass the query/vars to a library like cppgraphqlgen
    auto graphql_executor = [](const std::string& query, const std::string& operation_name, const nlohmann::json& variables) {
        nlohmann::json response;
        response["data"]["message"] = "Hello from Orbit GraphQL Adapter!";
        response["data"]["query"] = query;
        if (!operation_name.empty()) {
            response["data"]["operation_name"] = operation_name;
        }
        if (!variables.empty()) {
            response["data"]["variables"] = variables;
        }
        return response;
    };

    // Attach the GraphQL middleware to a POST and GET route
    app.post("/graphql", {graphql(graphql_executor)}, [](HttpRequest&, std::shared_ptr<ResponseWriter>) {});
    app.get("/graphql", {graphql(graphql_executor)}, [](HttpRequest&, std::shared_ptr<ResponseWriter>) {});

    std::cout << "GraphQL server running on http://localhost:" << config.port << "/graphql\n";
    std::cout << "Test it with:\n";
    std::cout << "  curl -X POST -H \"Content-Type: application/json\" -d '{\"query\": \"query { hello }\", \"variables\": {\"id\": 1}}' http://localhost:" << config.port << "/graphql\n";
    
    app.listen();
    return 0;
}
