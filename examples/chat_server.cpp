#include <orbit/server/App.hpp>
#include <iostream>
#include <unordered_set>
#include <mutex>

int main() {
    orbit::config::ServerConfig cfg;
    cfg.port = 8081;
    orbit::server::App app(cfg);

    std::unordered_set<orbit::http::websocket::WebSocketConnection*> clients;
    std::mutex clients_mutex;

    app.ws("/chat", [&clients, &clients_mutex](orbit::http::websocket::WebSocketConnection& ws) {
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.insert(&ws);
            std::cout << "Client connected. Total: " << clients.size() << std::endl;
        }
        ws.send("Welcome to the chat room!");

        ws.on_message([&ws, &clients, &clients_mutex](const std::string& msg) {
            std::cout << "Received: " << msg << std::endl;
            
            // Broadcast to all other clients
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto* client : clients) {
                if (client != &ws) {
                    client->send("Someone said: " + msg);
                }
            }
        });

        ws.on_close([&ws, &clients, &clients_mutex]() {
            std::lock_guard<std::mutex> lock(clients_mutex);
            clients.erase(&ws);
            std::cout << "Client disconnected. Total: " << clients.size() << std::endl;
        });
    });

    std::cout << "Starting Chat Server on ws://localhost:8081/chat" << std::endl;
    app.listen();

    return 0;
}
