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
#include <orbit/http/MultipartUpload.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/concurrency/Task.hpp>
#include <orbit/middleware/Proxy.hpp>
#include <iostream>
#include <csignal>

orbit::server::App* global_app = nullptr;

void handle_signal(int signum) {
    LOG_INFO("Received signal " << signum << ". Initiating graceful shutdown...");
    if (global_app) {
        global_app->stop();
    }
}

int main(int argc, char* argv[]) {
    try {
        auto config = orbit::config::ServerConfig::parse(argc, argv);
        
        orbit::server::App app(config);
        global_app = &app;

        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif

        // Global Middleware (Logging)
        app.use([](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> /*writer*/) {
            LOG_INFO("[Middleware] Received " << req.uri);
            return true;
        });


        // Setup global middlewares
        app.use(orbit::middleware::cors());
        app.use(orbit::middleware::Metrics::track());
        
        // Enable Prometheus metrics endpoint at /metrics
        app.enable_metrics();

        // Serve static files from "public" directory
        app.use(orbit::middleware::static_files(config.static_dir));

        // Rate Limiting (100 requests / 10 sec)
        app.use(orbit::middleware::rate_limit(100, std::chrono::seconds(10)));

        // Session Manager
        app.use(orbit::middleware::session("127.0.0.1", 6379));

        // API Group
        app.group("/api/v1", [&app](orbit::routing::Router& api) {
            api.add_route(orbit::http::HttpMethod::GET, "/users", [](const orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                orbit::http::HttpResponse res;
                res.body = "List of users";
                writer->send(std::move(res));
            });
            
            api.add_route(orbit::http::HttpMethod::POST, "/users", [](const orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                auto j = req.json();
                orbit::http::HttpResponse res;
                res.status(orbit::http::HttpStatus::Created);
                res.body = "User created: " + j.value("name", "Unknown");
                writer->send(std::move(res));
            });
            
            api.add_route(orbit::http::HttpMethod::GET, "/users/:id", [](const orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                orbit::http::HttpResponse res;
                res.body = "User ID: " + req.params.at("id");
                writer->send(std::move(res));
            });

            app.get("/api/mongo_test", [](orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) -> orbit::concurrency::Task {
            auto& thread_pool = global_app->get_thread_pool();
            orbit::database::MongoClient::Config mongo_cfg{"mongodb://localhost:27017", "testdb", "users"};
            orbit::database::MongoClient mongo(thread_pool, mongo_cfg);
            
            try {
                co_await mongo.insert_async("{\"name\": \"Orbit User\", \"role\": \"admin\"}");
                auto result = co_await mongo.find_async("{\"role\": \"admin\"}");
                
                std::string json = "[";
                for (size_t i = 0; i < result.documents.size(); ++i) {
                    json += result.documents[i];
                    if (i < result.documents.size() - 1) json += ",";
                }
                json += "]";
                
                orbit::http::HttpResponse res;
                res.headers["Content-Type"] = "application/json";
                res.body = json;
                writer->send(std::move(res));
            } catch (const std::exception& e) {
                orbit::http::HttpResponse res;
                res.status(orbit::http::HttpStatus::InternalServerError);
                res.body = e.what();
                writer->send(std::move(res));
            }
        });

        app.openapi().register_schema("Book", "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"},\"title\":{\"type\":\"string\"}}}");
        
        app.route("/api/books/:id", orbit::http::HttpMethod::GET)
            .summary("Get a book by ID")
            .description("Retrieves a specific book's details from the library.")
            .tag("Books")
            .res_body(200, "Book")
            .handler([](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                orbit::http::HttpResponse res;
                res.headers["Content-Type"] = "application/json";
                res.body = "{\"id\": " + req.params["id"] + ", \"title\": \"The C++ Programming Language\"}";
                writer->send(std::move(res));
            });

        app.openapi().register_schema("CreateUserRequest", "{\"type\":\"object\",\"properties\":{\"username\":{\"type\":\"string\"},\"age\":{\"type\":\"integer\"}}}");
        
        app.route("/api/users", orbit::http::HttpMethod::POST)
            .summary("Create a new user")
            .description("Creates a new user with the given username and age.")
            .tag("Users")
            .req_body("CreateUserRequest")
            .res_body(201, "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\"}}}")
            .handler([](orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                orbit::http::HttpResponse res;
                res.status(orbit::http::HttpStatus::Created);
                res.headers["Content-Type"] = "application/json";
                res.body = "{\"status\": \"user created successfully\"}";
                writer->send(std::move(res));
            });

        // Enable OpenAPI and Swagger UI at /docs
        app.enable_openapi("Orbit API", "1.0.0");

            api.add_stream_route(orbit::http::HttpMethod::POST, "/upload_multipart", [](orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
                // File parts are streamed to private temporary files as they arrive.
                orbit::http::receive_multipart(req, writer, [writer](orbit::http::MultipartUpload& upload) {
                    orbit::http::HttpResponse res;
                    if (!upload.ok()) {
                        res.status(orbit::http::HttpStatus::BadRequest).send(upload.error);
                    } else {
                        for (const auto& [name, value] : upload.fields) {
                            std::cout << "[Upload] Field: " << name << " = " << value << std::endl;
                        }
                        for (const auto& file : upload.files) {
                            std::cout << "[Upload] File saved: " << file.field << " -> " << file.path
                                      << " (" << file.filename << ", " << file.content_type << ")" << std::endl;
                        }
                        upload.discard(); // a real application would move the files into place
                        res.status(orbit::http::HttpStatus::OK).send("Upload processed successfully (Streaming)!");
                    }
                    writer->send(std::move(res));
                });
            });

            api.add_stream_route(orbit::http::HttpMethod::POST, "/upload", [](orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> res) {
                auto total_bytes = std::make_shared<size_t>(0);
                
                res->read_body_stream(
                    [total_bytes](std::string_view chunk) {
                        *total_bytes += chunk.size();
                    },
                    [res, total_bytes]() {
                        orbit::http::HttpResponse response;
                        response.status(orbit::http::HttpStatus::OK).send("Upload complete! Total bytes received: " + std::to_string(*total_bytes));
                        res->send(std::move(response));
                    }
                );
            });
            
            api.add_route(orbit::http::HttpMethod::GET, "/error", [](orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> /*res*/) {
                throw std::runtime_error("Simulated crash in API route!");
            });
            
            // Custom Error Handler for this router group
            api.on_error([](const std::exception& e, orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> res) {
                std::cout << "[Error Middleware] Caught exception: " << e.what() << std::endl;
                orbit::http::HttpResponse response;
                response.status(orbit::http::HttpStatus::InternalServerError);
                // Built as JSON, not concatenated: a quote in the message would break it.
                response.json(nlohmann::json{{"error", std::string("Custom API Error: ") + e.what()}});
                res->send(std::move(response));
            });
        });

        // Proxy/Load Balancer Group
        app.group("/proxy", [](orbit::routing::Router& proxy_router) {
            proxy_router.use(orbit::middleware::load_balancer({
                {"httpbin.org", 80},
                {"example.com", 80}
            }));
            
            proxy_router.add_route(orbit::http::HttpMethod::GET, "/*", [](orbit::http::HttpRequest&, std::shared_ptr<orbit::http::ResponseWriter>) {
                // Handled by middleware
            });
        });

        // Fluent routing
        app.get("/", [](const orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
            orbit::http::HttpResponse res;
            res.body = "<h1>Welcome to Orbit Framework!</h1>";
            res.headers["Content-Type"] = "text/html";
            writer->send(std::move(res));
        })
        .get("/api/data", [](const orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
            orbit::http::HttpResponse res;
            res.body = R"({"status": "success", "data": [1, 2, 3]})";
            res.headers["Content-Type"] = "application/json";
            writer->send(std::move(res));
        })
        .ws("/chat", [](orbit::http::websocket::WebSocketConnection& ws) {
            ws.on_message([&ws](const std::string& msg) {
                std::cout << "Received WS Message: " << msg << std::endl;
                ws.send("Echo: " + msg);
            });
            ws.on_close([]() {
                std::cout << "WS connection closed." << std::endl;
            });
        })
        .get("/sse", [](const orbit::http::HttpRequest& /*req*/, std::shared_ptr<orbit::http::ResponseWriter> writer) {
            orbit::http::HttpResponse res;
            res.status(orbit::http::HttpStatus::OK);
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
        .get("/users/:id", [](const orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
            std::string user_id = req.params.at("id");
            nlohmann::json j = {
                {"user_id", user_id},
                {"name", "John Doe"}
            };
            orbit::http::HttpResponse res;
            res.json(j);
            writer->send(std::move(res));
        })
        .post("/api/echo", [](const orbit::http::HttpRequest& req, std::shared_ptr<orbit::http::ResponseWriter> writer) {
            auto j = req.json();
            j["received"] = true;
            orbit::http::HttpResponse res;
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
