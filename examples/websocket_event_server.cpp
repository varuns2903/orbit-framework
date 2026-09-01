#include <orbit/server/App.hpp>
#include <orbit/config/Config.hpp>
#include <orbit/websocket/EventRouter.hpp>
#include <orbit/http/json.hpp>
#include <iostream>

using namespace server;
using namespace websocket;

// 1. Define your Session State (attached to every connected socket)
struct PlayerSession {
    std::string username = "Guest";
    int score = 0;
};

// 2. Define the incoming/outgoing Event Structs
struct ChatMessage {
    std::string text;
};

struct MoveEvent {
    int position;
};

// 3. Tell JSON how to serialize them
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChatMessage, text)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MoveEvent, position)

int main() {
    config::ServerConfig config;
    config.port = 8084;
    App app(config);

    // Create the strongly-typed router
    EventRouter<PlayerSession> game_router;

    game_router.on_connect([](auto& ws) {
        std::cout << "Client connected: " << ws.id() << std::endl;
        ws.join("lobby");
        ws.emit("welcome", std::string("Welcome to Orbit Games!"));
    });

    game_router.on_disconnect([](auto& ws) {
        std::cout << "Client disconnected: " << ws.id() << std::endl;
        ws.to("lobby").emit("sys_msg", ws.session().username + " left the building.");
    });

    // Strongly-Typed Event: No JSON parsing needed!
    game_router.on<ChatMessage>("chat", [](auto& ws, const ChatMessage& msg) {
        std::cout << ws.session().username << " says: " << msg.text << std::endl;
        
        // Broadcast to everyone in the lobby
        ws.to("lobby").emit("chat_broadcast", std::string(ws.session().username + ": " + msg.text));
    });

    game_router.on<MoveEvent>("move", [](auto& ws, const MoveEvent& move) {
        std::cout << "Player moved to " << move.position << std::endl;
        ws.session().score += 10;
        ws.emit("score_update", ws.session().score);
    });

    // Attach to the application
    game_router.attach(app, "/game");

    std::cout << "Starting WebSocket Event Router Server on ws://localhost:8084/game\n";
    std::cout << "Use a websocket client (like wscat) to connect.\n";
    std::cout << "Send: {\"event\": \"chat\", \"data\": {\"text\": \"Hello!\"}}\n";
    std::cout << "Send: {\"event\": \"move\", \"data\": {\"position\": 4}}\n";
    
    app.listen();

    return 0;
}
