#pragma once

#include <orbit/config/Config.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace orbit::test {

// Multiplier for client-side timeouts in tests. CI sets
// ORBIT_TEST_TIMEOUT_SCALE for the run under valgrind, which is many times
// slower (about 100 KB/s over QUIC); unset or invalid, it is 1.
inline long timeout_scale() {
    const char* value = std::getenv("ORBIT_TEST_TIMEOUT_SCALE");
    if (value == nullptr) return 1;
    const long scale = std::strtol(value, nullptr, 10);
    return scale > 0 ? scale : 1;
}

// The event engine the integration tests serve on. Unset, it is the library's
// default; ORBIT_TEST_ENGINE=iouring (or epoll) runs the same tests on another
// engine, which is how CI covers IoUringProactor. An unknown name fails the
// test instead of silently falling back to the default engine.
inline orbit::config::EventEngine test_engine() {
    const char* name = std::getenv("ORBIT_TEST_ENGINE");
    if (name == nullptr || *name == '\0') return orbit::config::ServerConfig{}.engine;
    const std::string engine = name;
    if (engine == "epoll") return orbit::config::EventEngine::Epoll;
    if (engine == "iouring") return orbit::config::EventEngine::IoUring;
    throw std::invalid_argument("ORBIT_TEST_ENGINE must be 'epoll' or 'iouring', not '" + engine + "'");
}

// A ServerConfig for a test server. Integration tests start from this rather
// than a default-constructed ServerConfig so they follow ORBIT_TEST_ENGINE.
inline orbit::config::ServerConfig server_config() {
    orbit::config::ServerConfig cfg;
    cfg.engine = test_engine();
    return cfg;
}

} // namespace orbit::test
