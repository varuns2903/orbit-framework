#pragma once
#include <orbit/server/App.hpp>
#include <orbit/http/WebSocketConnection.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <memory>
#include <iostream>

namespace websocket {

struct EmptySession {};

template <typename SessionType = EmptySession>
class EventRouter;

/**
 * @brief Represents a connected client in the EventRouter.
 */
template <typename SessionType>
class EventSocket {
public:
    EventSocket(http::websocket::WebSocketConnection& raw, EventRouter<SessionType>& router, std::string id)
        : raw_(raw), router_(router), id_(std::move(id)) {}

    /**
     * @brief Emits an event with strongly-typed data back to this specific client.
     */
    template <typename T>
    void emit(const std::string& event, const T& data) {
        nlohmann::json payload = {{"event", event}, {"data", data}};
        raw_.send(payload.dump());
    }

    /**
     * @brief Joins a specific room/channel.
     */
    void join(const std::string& room);
    
    /**
     * @brief Leaves a specific room/channel.
     */
    void leave(const std::string& room);

    /**
     * @brief Returns a broadcaster to emit messages to everyone in the room.
     */
    auto to(const std::string& room);

    /**
     * @brief The strongly-typed session state attached to this connection.
     */
    SessionType& session() { return session_; }

    const std::string& id() const { return id_; }
    
    // Internal access
    http::websocket::WebSocketConnection& raw() { return raw_; }

private:
    http::websocket::WebSocketConnection& raw_;
    EventRouter<SessionType>& router_;
    std::string id_;
    SessionType session_;
    std::unordered_set<std::string> rooms_;
    
    friend class EventRouter<SessionType>;
};

/**
 * @brief A Socket.IO style event router for WebSockets with Room and Session support.
 */
template <typename SessionType>
class EventRouter {
public:
    using SocketPtr = std::shared_ptr<EventSocket<SessionType>>;

    /**
     * @brief Registers a strongly-typed event handler.
     * 
     * @example 
     * router.on<MoveEvent>("move", [](auto& ws, const MoveEvent& data) { ... });
     */
    template <typename PayloadType>
    void on(const std::string& event, std::function<void(EventSocket<SessionType>&, const PayloadType&)> handler) {
        handlers_[event] = [handler, event](EventSocket<SessionType>& ws, const nlohmann::json& raw_data) {
            try {
                PayloadType data = raw_data.get<PayloadType>();
                handler(ws, data);
            } catch (const std::exception& e) {
                // Ignore or log bad payload
                std::cerr << "[EventRouter] Invalid payload for event '" << event << "': " << e.what() << "\n";
            }
        };
    }

    /**
     * @brief Registers an untyped event handler (no data expected).
     */
    void on(const std::string& event, std::function<void(EventSocket<SessionType>&)> handler) {
        handlers_[event] = [handler](EventSocket<SessionType>& ws, const nlohmann::json&) {
            handler(ws);
        };
    }

    /**
     * @brief Registers a connection handler.
     */
    void on_connect(std::function<void(EventSocket<SessionType>&)> handler) {
        on_connect_ = std::move(handler);
    }

    /**
     * @brief Registers a disconnection handler.
     */
    void on_disconnect(std::function<void(EventSocket<SessionType>&)> handler) {
        on_disconnect_ = std::move(handler);
    }

    /**
     * @brief Attaches this EventRouter to the Orbit App at a specific path.
     */
    void attach(server::App& app, const std::string& path) {
        app.ws(path, [this](http::websocket::WebSocketConnection& raw_ws) {
            
            // Simple UUID generation using counter and pointer for uniqueness
            static std::atomic<uint64_t> counter = 0;
            std::string id = "ws_" + std::to_string(++counter) + "_" + std::to_string(reinterpret_cast<uint64_t>(&raw_ws));
            
            auto es = std::make_shared<EventSocket<SessionType>>(raw_ws, *this, id);
            
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sockets_[id] = es;
            }

            if (on_connect_) on_connect_(*es);

            raw_ws.on_message([this, id](const std::string& msg) {
                try {
                    auto j = nlohmann::json::parse(msg);
                    if (!j.contains("event")) return;
                    
                    std::string event_name = j["event"];
                    nlohmann::json data = j.contains("data") ? j["data"] : nlohmann::json(nullptr);
                    
                    auto socket = get_socket(id);
                    if (socket) {
                        auto it = handlers_.find(event_name);
                        if (it != handlers_.end()) {
                            it->second(*socket, data);
                        }
                    }
                } catch (...) {
                    // Ignore malformed JSON
                }
            });

            raw_ws.on_close([this, id]() {
                auto socket = get_socket(id);
                if (socket && on_disconnect_) on_disconnect_(*socket);
                
                std::lock_guard<std::mutex> lock(mutex_);
                if (socket) {
                    for (const auto& room : socket->rooms_) {
                        rooms_[room].erase(id);
                    }
                }
                sockets_.erase(id);
            });
        });
    }
    
    // Broadcasting helper class
    struct RoomBroadcaster {
        EventRouter& router;
        std::string room;
        
        template <typename T>
        void emit(const std::string& event, const T& data) {
            nlohmann::json payload = {{"event", event}, {"data", data}};
            std::string msg = payload.dump();
            router.broadcast_to_room(room, msg);
        }
    };
    
    RoomBroadcaster to(const std::string& room) {
        return RoomBroadcaster{*this, room};
    }
    
    // Internal API for EventSocket
    void join_room(const std::string& id, const std::string& room) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sockets_.count(id)) {
            sockets_[id]->rooms_.insert(room);
            rooms_[room].insert(id);
        }
    }
    
    void leave_room(const std::string& id, const std::string& room) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sockets_.count(id)) {
            sockets_[id]->rooms_.erase(room);
            rooms_[room].erase(id);
        }
    }

    void broadcast_to_room(const std::string& room, const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rooms_.count(room)) {
            for (const std::string& id : rooms_[room]) {
                if (sockets_.count(id)) {
                    sockets_[id]->raw().send(msg);
                }
            }
        }
    }

private:
    SocketPtr get_socket(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sockets_.find(id);
        if (it != sockets_.end()) return it->second;
        return nullptr;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, SocketPtr> sockets_;
    std::unordered_map<std::string, std::unordered_set<std::string>> rooms_;
    std::unordered_map<std::string, std::function<void(EventSocket<SessionType>&, const nlohmann::json&)>> handlers_;
    
    std::function<void(EventSocket<SessionType>&)> on_connect_;
    std::function<void(EventSocket<SessionType>&)> on_disconnect_;
};

template <typename SessionType>
void EventSocket<SessionType>::join(const std::string& room) {
    router_.join_room(id_, room);
}

template <typename SessionType>
void EventSocket<SessionType>::leave(const std::string& room) {
    router_.leave_room(id_, room);
}

template <typename SessionType>
auto EventSocket<SessionType>::to(const std::string& room) {
    return router_.to(room);
}

} // namespace websocket
