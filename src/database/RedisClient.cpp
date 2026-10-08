#include <orbit/database/RedisClient.hpp>
#include <orbit/utils/Logger.hpp>
#include <orbit/network/PlatformSocket.hpp>
#include <sstream>
#ifndef _WIN32
#include <netdb.h>
#endif

namespace orbit::database {

RedisClient::RedisClient(const std::string& host, int port) : host_(host), port_(port) {
    connect();
}

RedisClient::~RedisClient() {
    disconnect();
}

bool RedisClient::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connect_locked();
}

bool RedisClient::connect_locked() {
    if (fd_ != -1) return true;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    std::string port_str = std::to_string(port_);
    if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0 || !result) {
        LOG_ERROR("Redis DNS resolution failed for " << host_);
        return false;
    }
    sockaddr_in addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    freeaddrinfo(result);

    fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd_ < 0) {
        fd_ = -1;
        return false;
    }

    // Bound every blocking call so a stalled Redis cannot pin worker threads.
#ifdef _WIN32
    DWORD timeout_ms = 2000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{2, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // Connect synchronously (this runs on worker threads, so it's okay)
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_locked();
        LOG_ERROR("Failed to connect to Redis at " << host_ << ":" << port_);
        return false;
    }

    LOG_INFO("Connected to Redis successfully");
    return true;
}

void RedisClient::close_locked() {
    if (fd_ != -1) {
        network::close_socket(fd_);
        fd_ = -1;
    }
}

void RedisClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

std::string RedisClient::send_command(const std::vector<std::string>& args, bool* no_value) {
    std::ostringstream oss;
    oss << "*" << args.size() << "\r\n";
    for (const auto& arg : args) {
        oss << "$" << arg.size() << "\r\n" << arg << "\r\n";
    }
    
    std::string req = oss.str();
    
    if (no_value) *no_value = true;

    // connect_locked() rather than connect(): mutex_ is not recursive, and
    // re-locking it here deadlocked every caller after the first disconnect.
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ == -1 && !connect_locked()) return "";

    size_t total_sent = 0;
    while (total_sent < req.size()) {
        ssize_t s = send(fd_, req.data() + total_sent, req.size() - total_sent, 0);
        if (s <= 0) {
            close_locked();
            return ""; // Connection dropped
        }
        total_sent += static_cast<size_t>(s);
    }

    bool ok = false;
    bool nil = true;
    std::string response = read_response(ok, nil);
    if (no_value) *no_value = !ok || nil;
    if (!ok) {
        // A failed or partial read leaves the stream at an unknown position;
        // start over on a fresh connection next time.
        close_locked();
    }
    return response;
}

std::string RedisClient::read_response(bool& ok, bool& no_value) {
    ok = false;
    no_value = false;
    // A simple, unoptimized RESP reader for synchronous reading.
    char c;
    std::string line;
    
    // Read the first line to determine response type
    while (recv(fd_, &c, 1, 0) == 1) {
        line += c;
        if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n") {
            break;
        }
    }

    if (line.size() < 3 || line.substr(line.size() - 2) != "\r\n") return "";

    char type = line[0];
    line = line.substr(1, line.size() - 3); // Remove type char and \r\n

    if (type == '+') {
        // Simple string
        ok = true;
        return line;
    } else if (type == '-') {
        // Error reply: the stream is still in sync.
        LOG_ERROR("Redis Error: " << line);
        ok = true;
        no_value = true;
        return "";
    } else if (type == ':') {
        // Integer
        ok = true;
        return line;
    } else if (type == '$') {
        // Bulk string
        long long len = 0;
        try {
            len = std::stoll(line);
        } catch (...) {
            return "";
        }
        if (len == -1) {
            ok = true;
            no_value = true;
            return "";
        }
        if (len < 0 || len > 512LL * 1024 * 1024) return "";
        
        std::string bulk;
        bulk.resize(static_cast<size_t>(len));
        
        size_t total_read = 0;
        while (total_read < static_cast<size_t>(len)) {
            ssize_t r = recv(fd_, &bulk[0] + total_read, static_cast<size_t>(len) - total_read, 0);
            if (r <= 0) return "";
            total_read += static_cast<size_t>(r);
        }
        
        // consume trailing \r\n
        char crlf[2];
        if (recv(fd_, crlf, 2, MSG_WAITALL) != 2) return "";
        
        ok = true;
        return bulk;
    }

    // Arrays and other types are not used by this client; the stream can't be
    // resynchronised without parsing them, so report failure.
    return "";
}

std::string RedisClient::ping() {
    return send_command({"PING"});
}

bool RedisClient::set(const std::string& key, const std::string& value, int expire_seconds) {
    if (expire_seconds > 0) {
        std::string res = send_command({"SET", key, value, "EX", std::to_string(expire_seconds)});
        return res == "OK";
    } else {
        std::string res = send_command({"SET", key, value});
        return res == "OK";
    }
}

std::optional<std::string> RedisClient::get(const std::string& key) {
    // A missing key is a nil reply; a key holding "" is an empty bulk string.
    bool no_value = true;
    std::string res = send_command({"GET", key}, &no_value);
    if (no_value) return std::nullopt;
    return res;
}

long long RedisClient::incr(const std::string& key) {
    std::string res = send_command({"INCR", key});
    if (res.empty()) return 0;
    try {
        return std::stoll(res);
    } catch (...) {
        return 0;
    }
}

long long RedisClient::incr_with_expiry(const std::string& key, int seconds) {
    static const char* kScript =
        "local n = redis.call('INCR', KEYS[1]) "
        "if n == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
        "return n";
    std::string res = send_command({"EVAL", kScript, "1", key, std::to_string(seconds)});
    if (res.empty()) return 0;
    try {
        return std::stoll(res);
    } catch (...) {
        return 0;
    }
}

bool RedisClient::expire(const std::string& key, int seconds) {
    std::string res = send_command({"EXPIRE", key, std::to_string(seconds)});
    return res == "1";
}

bool RedisClient::del(const std::string& key) {
    std::string res = send_command({"DEL", key});
    return res == "1";
}

} // namespace database
