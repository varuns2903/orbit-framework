# Consumer smoke test for the vcpkg port: find the installed package, link
# OrbitFramework::core, and serve one request. Built by .github/workflows/vcpkg-port.yml.
cmake_minimum_required(VERSION 3.20)
project(orbit_vcpkg_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OrbitFramework 2.0 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE OrbitFramework::core)
