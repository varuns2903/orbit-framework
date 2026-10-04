#include <orbit/server/App.hpp>
#include <orbit/middleware/StaticFiles.hpp>
#include <orbit/middleware/Cors.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <orbit/middleware/RateLimiter.hpp>
#include <orbit/middleware/SessionManager.hpp>
#include <orbit/middleware/JwtAuth.hpp>
#include <orbit/middleware/Metrics.hpp>
#include <orbit/database/RedisClient.hpp>
#include <orbit/database/PostgresClient.hpp>
#include <orbit/database/MysqlClient.hpp>
#include <orbit/database/MongoClient.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/http/MultipartForm.hpp>
#include <orbit/http/MultipartStreamParser.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <iostream>
#include <csignal>

server::App* global_app = nullptr;

void handle_signal(int signum) {
    LOG_INFO("Received signal " << signum << ". Initiating graceful shutdown...");
    if (global_app) {
        global_app->stop();
    }
}

int main(int argc, char* argv[]) {
    try {
        auto config = config::ServerConfig::parse(argc, argv);
        
        server::App app(config);
        global_app = &app;

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif

        // Global Middleware (Logging)
        app.use([](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> /*writer*/) {
            LOG_INFO("[Middleware] Received " << req.uri);
            return true;
        });


        // Setup global middlewares
        app.use(middleware::cors());
        app.use(middleware::Metrics::track());
        
        // Enable Prometheus metrics endpoint at /metrics
        app.enable_metrics();

        // Serve static files from "public" directory
        app.use(middleware::static_files(config.static_dir));

        // Rate Limiting (100 requests / 10 sec)
        app.use(middleware::rate_limit(100, std::chrono::seconds(10)));

        // Session Manager
        app.use(middleware::session("127.0.0.1", 6379));

        // API Group
        app.group("/api/v1", [&app](routing::Router& api) {
            api.add_route(http::HttpMethod::GET, "/users", [](const http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
                http::HttpResponse res;
                res.body = "List of users";
                writer->send(std::move(res));
            });
            
            api.add_route(http::HttpMethod::POST, "/users", [](const http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
                auto j = req.json();
                http::HttpResponse res;
                res.status(http::HttpStatus::Created);
                res.body = "User created: " + j.value("name", "Unknown");
                writer->send(std::move(res));
            });
            
            api.add_route(http::HttpMethod::GET, "/users/:id", [](const http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
                http::HttpResponse res;
                res.body = "User ID: " + req.params.at("id");
                writer->send(std::move(res));
            });

            app.get("/api/mongo_test", [](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) -> concurrency::Task {
            auto& thread_pool = global_app->get_thread_pool();
            database::MongoClient::Config mongo_cfg{"mongodb://localhost:27017", "testdb", "users"};
            database::MongoClient mongo(thread_pool, mongo_cfg);
            
            try {
                co_await mongo.insert_async("{\"name\": \"Orbit User\", \"role\": \"admin\"}");
                auto result = co_await mongo.find_async("{\"role\": \"admin\"}");
                
                std::string json = "[";
                for (size_t i = 0; i < result.documents.size(); ++i) {
                    json += result.documents[i];
                    if (i < result.documents.size() - 1) json += ",";
                }
                json += "]";
                
                http::HttpResponse res;
                res.headers["Content-Type"] = "application/json";
                res.body = json;
                writer->send(std::move(res));
            } catch (const std::exception& e) {
                http::HttpResponse res;
                res.status(http::HttpStatus::InternalServerError);
                res.body = e.what();
                writer->send(std::move(res));
            }
        });

        app.openapi().register_schema("Book", "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"title\":{\"type\":\"string\"}}}");
        
        app.route("/api/books/:id", http::HttpMethod::GET)
            .summary("Get a book by ID")
            .description("Retrieves a specific book's details from the library.")
            .tag("Books")
            .res_body(200, "Book")
            .handler([](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
                http::HttpResponse res;
                res.headers["Content-Type"] = "application/json";
                res.body = "{\"id\": " + req.params["id"] + ", \"title\": \"The C++ Programming Language\"}";
                writer->send(std::move(res));
            });

        app.openapi().register_schema("CreateUserRequest", "{\"type\":\"object\",\"properties\":{\"username\":{\"type\":\"string\"},\"age\":{\"type\":\"integer\"}}}");
        
        app.route("/api/users", http::HttpMethod::POST)
            .summary("Create a new user")
            .description("Creates a new user with the given username and age.")
            .tag("Users")
            .req_body("CreateUserRequest")
            .res_body(201, "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}")
            .handler([](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
                http::HttpResponse res;
                res.status(http::HttpStatus::Created);
                res.headers["Content-Type"] = "application/json";
                res.body = "{\"status\": \"user created successfully\"}";
                writer->send(std::move(res));
            });

        // Enable OpenAPI and Swagger UI at /docs
        app.enable_openapi("Orbit API", "1.0.0");

            api.add_stream_route(http::HttpMethod::POST, "/upload_multipart", [](http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
                auto ct_it = req.headers.find("Content-Type");
                if (ct_it == req.headers.end() || !ct_it->second.starts_with("multipart/form-data")) {
                    http::HttpResponse res;
                    res.status(http::HttpStatus::BadRequest).send("Expected multipart/form-data");
                    writer->send(std::move(res));
                    return;
                }

                std::string_view ct = ct_it->second;
                size_t b_pos = ct.find("boundary=");
                if (b_pos == std::string::npos) {
                    http::HttpResponse res;
                    res.status(http::HttpStatus::BadRequest).send("No boundary found");
                    writer->send(std::move(res));
                    return;
                }
                
                std::string boundary(ct.substr(b_pos + 9));
                
                // We use a shared_ptr for the parser because the callbacks might outlive this function context
                auto parser = std::make_shared<http::MultipartStreamParser>(
                    boundary,
                    [](const std::string& name, const std::string& value) {
                        std::cout << "[Upload] Field: " << name << " = " << value << std::endl;
                    },
                    [](const std::string& name, const std::string& filename, const std::string& content_type, const std::string& tmp_filepath) {
                        std::cout << "[Upload] File saved: " << name << " -> " << tmp_filepath 
                                  << " (" << filename << ", " << content_type << ")" << std::endl;
                    }
                );

                writer->read_body_stream(
                    [parser](std::string_view chunk) {
                        parser->feed(chunk);
                    },
                    [writer, parser]() {
                        parser->end();
                        http::HttpResponse res;
                        res.status(http::HttpStatus::OK).send("Upload processed successfully (Streaming)!");
                        writer->send(std::move(res));
                    }
                );
            });

            api.add_stream_route(http::HttpMethod::POST, "/upload", [](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> res) {
                auto total_bytes = std::make_shared<size_t>(0);
                
                res->read_body_stream(
                    [total_bytes](std::string_view chunk) {
                        *total_bytes += chunk.size();
                    },
                    [res, total_bytes]() {
                        http::HttpResponse response;
                        response.status(http::HttpStatus::OK).send("Upload complete! Total bytes received: " + std::to_string(*total_bytes));
                        res->send(std::move(response));
                    }
                );
            });
            
            api.add_route(http::HttpMethod::GET, "/error", [](http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> /*res*/) {
                throw std::runtime_error("Simulated crash in API route!");
            });
            
            // Custom Error Handler for this router group
            api.on_error([](const std::exception& e, http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> res) {
                std::cout << "[Error Middleware] Caught exception: " << e.what() << std::endl;
                http::HttpResponse response;
                response.status(http::HttpStatus::InternalServerError);
                response.headers["Content-Type"] = "application/json";
                response.send("{\"error\": \"Custom API Error: " + std::string(e.what()) + "\"}");
                res->send(std::move(response));
            });
        });

        // Proxy/Load Balancer Group
        app.group("/proxy", [](routing::Router& proxy_router) {
            proxy_router.use(middleware::load_balancer({
                {"httpbin.org", 80},
                {"example.com", 80}
            }));
            
            proxy_router.add_route(http::HttpMethod::GET, "/*", [](http::HttpRequest&, std::shared_ptr<http::ResponseWriter>) {
                // Handled by middleware
            });
        });

        // Fluent routing
        app.get("/", [](const http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
            http::HttpResponse res;
            res.body = "<h1>Welcome to Orbit Framework!</h1>";
            res.headers["Content-Type"] = "text/html";
            writer->send(std::move(res));
        })
        .get("/api/data", [](const http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
            http::HttpResponse res;
            res.body = R"({"status": "success", "data": [1, 2, 3]})";
            res.headers["Content-Type"] = "application/json";
            writer->send(std::move(res));
        })
        .ws("/chat", [](http::websocket::WebSocketConnection& ws) {
            ws.on_message([&ws](const std::string& msg) {
                std::cout << "Received WS Message: " << msg << std::endl;
                ws.send("Echo: " + msg);
            });
            ws.on_close([]() {
                std::cout << "WS connection closed." << std::endl;
            });
        })
        .get("/sse", [](const http::HttpRequest& /*req*/, std::shared_ptr<http::ResponseWriter> writer) {
            http::HttpResponse res;
            res.status(http::HttpStatus::OK);
            res.headers["Content-Type"] = "text/event-stream";
            res.headers["Cache-Control"] = "no-cache";
            res.headers["Connection"] = "keep-alive";
            res.headers["Transfer-Encoding"] = "chunked";
            
            // Send headers first
            writer->send_headers(res);
            
            // Send initial SSE event
            writer->send_sse_event("Connected to Orbit SSE!", "hello");
            
            // We'll simulate pushing some background events by spinning up a thread
            // In a real application, you'd register this writer in a connection manager
            std::thread([writer]() {
                for (int i = 0; i < 5; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    writer->send_sse_event("Tick " + std::to_string(i), "ping");
                }
                writer->end(); // Ends the chunked stream
            }).detach();
        })
        .get("/users/:id", [](const http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
            std::string user_id = req.params.at("id");
            nlohmann::json j = {
                {"user_id", user_id},
                {"name", "John Doe"}
            };
            http::HttpResponse res;
            res.json(j);
            writer->send(std::move(res));
        })
        .post("/api/echo", [](const http::HttpRequest& req, std::shared_ptr<http::ResponseWriter> writer) {
            auto j = req.json();
            j["received"] = true;
            http::HttpResponse res;
            res.json(j);
            writer->send(std::move(res));
        });

        app.listen();
        
        global_app = nullptr;

    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: " << e.what());
        return 1;
    }

    LOG_INFO("Server shutdown complete. Goodbye!");
    return 0;
}
