// Echoes every WebSocket message back with the same type (text or binary).
// Used by the Autobahn conformance job: .github/workflows/autobahn.yml.
//
//   ./websocket_echo_server [--port 9001]
#include <orbit/server/App.hpp>

int main(int argc, char* argv[]) {
    orbit::config::ServerConfig config = orbit::config::ServerConfig::parse(argc, argv);
    orbit::server::App app(config);

    app.ws("/", [](orbit::http::websocket::WebSocketConnection& ws) {
        ws.set_max_message_size(64 * 1024 * 1024); // Autobahn sends messages up to 16 MiB
        ws.on_message([&ws](const std::string& text) { ws.send(text); });
        ws.on_binary_message([&ws](const std::string& data) { ws.send_binary(data); });
    });

    app.listen();
    return 0;
}
