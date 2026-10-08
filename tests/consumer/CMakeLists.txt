# An application that pulls Orbit in with FetchContent, as `orbit new --fetch`
# sets it up, built from this checkout instead of a tag. Built in Debug by
# .github/workflows/consumer.yml (#185):
#   cmake -B build -S tests/consumer -DCMAKE_BUILD_TYPE=Debug \
#         -DORBIT_SOURCE_DIR=$PWD -DVCPKG_MANIFEST_DIR=$PWD -DCMAKE_TOOLCHAIN_FILE=...
cmake_minimum_required(VERSION 3.20)
project(orbit_fetchcontent_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT ORBIT_SOURCE_DIR)
    message(FATAL_ERROR "Set ORBIT_SOURCE_DIR to the Orbit checkout")
endif()

include(FetchContent)
FetchContent_Declare(OrbitFramework SOURCE_DIR "${ORBIT_SOURCE_DIR}")
FetchContent_MakeAvailable(OrbitFramework)

# Same program as the vcpkg port's consumer test: serve one request, check it.
add_executable(consumer "${ORBIT_SOURCE_DIR}/packaging/vcpkg-port/test/main.cpp")
target_link_libraries(consumer PRIVATE OrbitFramework::core)
