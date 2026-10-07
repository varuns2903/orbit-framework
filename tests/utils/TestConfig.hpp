#pragma once

#include <orbit/config/Config.hpp>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace orbit::test {

// The event engine the integration tests serve on. Unset, it is the library's
// default; ORBIT_TEST_ENGINE=iouring (or epoll) runs the same tests on another
// engine, which is how CI covers IoUringProactor. An unknown name fails the
// test instead of silently falling back to the default engine.
inline config::EventEngine test_engine() {
    const char* name = std::getenv("ORBIT_TEST_ENGINE");
    if (name == nullptr || *name == '\0') return config::ServerConfig{}.engine;
    const std::string engine = name;
    if (engine == "epoll") return config::EventEngine::Epoll;
    if (engine == "iouring") return config::EventEngine::IoUring;
    throw std::invalid_argument("ORBIT_TEST_ENGINE must be 'epoll' or 'iouring', not '" + engine + "'");
}

// A ServerConfig for a test server. Integration tests start from this rather
// than a default-constructed ServerConfig so they follow ORBIT_TEST_ENGINE.
inline config::ServerConfig server_config() {
    config::ServerConfig cfg;
    cfg.engine = test_engine();
    return cfg;
}

} // namespace orbit::test
