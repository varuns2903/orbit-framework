cmake_minimum_required(VERSION 3.20)
project(OrbitFramework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings to catch issues early
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX /wd4100 /wd4244 /wd4267 /wd4018 /wd4996")
    add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS ssize_t=intptr_t)
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic -Wshadow
        -Wconversion -Wsign-conversion -Wcast-align
    )
endif()
# --- Modular Features ---
option(ORBIT_ENABLE_REDIS "Enable Redis database support" ON)
option(ORBIT_ENABLE_POSTGRES "Enable PostgreSQL database support" ON)
option(ORBIT_ENABLE_MARIADB "Enable MariaDB database support" ON)
option(ORBIT_ENABLE_MONGODB "Enable MongoDB database support" ON)
option(ORBIT_ENABLE_HTTP3 "Enable HTTP/3 and QUIC support" ON)
option(ORBIT_ENABLE_GRPC "Enable gRPC support" OFF)


# Optional sanitizers for debug builds to catch memory leaks and UB
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers" ON)
if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)

    add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
if(ORBIT_ENABLE_POSTGRES)
    find_package(PostgreSQL REQUIRED)
endif()
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)

if(ORBIT_ENABLE_MARIADB)
    find_package(unofficial-libmariadb CONFIG QUIET)
    if(unofficial-libmariadb_FOUND)
        set(MARIADB_LIBRARIES unofficial::libmariadb)
    else()
        pkg_search_module(MARIADB REQUIRED libmariadb mariadb)
    endif()
endif()

if(ORBIT_ENABLE_MONGODB)
    find_package(mongoc-1.0 CONFIG QUIET)
    if(NOT mongoc-1.0_FOUND)
        find_package(mongoc CONFIG QUIET)
    endif()

    if(TARGET mongo::shared)
        set(MONGOC_LIBRARIES mongo::shared)
    elseif(TARGET mongo::static)
        set(MONGOC_LIBRARIES mongo::static)
    elseif(TARGET mongo::mongoc_shared)
        set(MONGOC_LIBRARIES mongo::mongoc_shared)
    elseif(TARGET mongo::mongoc_static)
        set(MONGOC_LIBRARIES mongo::mongoc_static)
    elseif(TARGET mongoc::mongoc)
        set(MONGOC_LIBRARIES mongoc::mongoc)
    else()
        pkg_search_module(MONGOC REQUIRED libmongoc-1.0 mongoc-1.0 mongoc2)
    endif()
endif()

if(ORBIT_ENABLE_REDIS)
    find_package(hiredis CONFIG QUIET)
    if(hiredis_FOUND)
        set(HIREDIS_LIBRARIES hiredis::hiredis)
    else()
        pkg_search_module(HIREDIS REQUIRED hiredis)
    endif()
endif()

pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

if(ORBIT_ENABLE_HTTP3)
    pkg_check_modules(NGTCP2 REQUIRED IMPORTED_TARGET libngtcp2)
    pkg_search_module(NGTCP2_CRYPTO REQUIRED IMPORTED_TARGET libngtcp2_crypto_openssl libngtcp2_crypto_ossl libngtcp2_crypto_quictls)
    if(NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_ossl" OR NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_openssl")
        add_compile_definitions(USE_NGTCP2_CRYPTO_OSSL=1)
    elseif(NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_quictls")
        add_compile_definitions(USE_NGTCP2_CRYPTO_QUICTLS=1)
    endif()
    pkg_check_modules(NGHTTP3 REQUIRED IMPORTED_TARGET libnghttp3)
endif()

include(FetchContent)
FetchContent_Declare(
  inja
  GIT_REPOSITORY https://github.com/pantor/inja.git
  GIT_TAG        v3.4.0
)
set(INJA_BUILD_TESTS OFF CACHE BOOL "Disable inja tests" FORCE)
set(BUILD_BENCHMARK OFF CACHE BOOL "Disable inja benchmark" FORCE)
FetchContent_MakeAvailable(inja)


# Core static library
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(PROACTOR_SOURCES src/network/KqueueProactor.cpp)
elseif(WIN32)
    set(PROACTOR_SOURCES src/network/IocpProactor.cpp)
else()
    set(PROACTOR_SOURCES src/network/EpollProactor.cpp src/network/IoUringProactor.cpp)
endif()

set(SERVER_SOURCES
    src/network/PlatformSocket.cpp
    src/network/Socket.cpp
    src/network/ConnectionPool.cpp
    src/network/TlsContext.cpp
    src/server/Listener.cpp
    src/config/Config.cpp
    src/utils/Logger.cpp
    src/server/EventLoop.cpp
    src/server/Connection.cpp
    src/utils/PrometheusRegistry.cpp
    src/server/ConnectionManager.cpp
    src/server/TimerManager.cpp
    src/server/GrpcServer.cpp
    ${PROACTOR_SOURCES}
    src/network/UdpSocket.cpp
    src/concurrency/ThreadPool.cpp
    src/http/HttpParser.cpp
    src/http/HttpResponse.cpp
    src/http/MultipartForm.cpp
    src/http/MultipartStreamParser.cpp
    src/http/WebSocketConnection.cpp
    src/http/Http2Session.cpp
    src/routing/Router.cpp
    src/server/App.cpp
    src/middleware/StaticFiles.cpp
    src/middleware/Compress.cpp
    src/middleware/JwtAuth.cpp
    src/middleware/Cors.cpp
    src/middleware/Proxy.cpp
    src/middleware/RateLimiter.cpp
    src/middleware/DistributedRateLimiter.cpp
    src/middleware/SessionManager.cpp
    src/middleware/Validation.cpp
    src/middleware/Metrics.cpp
    src/middleware/Csrf.cpp
    src/middleware/OAuth2.cpp
    src/openapi/OpenApi.cpp
)

if(ORBIT_ENABLE_HTTP3)
    list(APPEND SERVER_SOURCES 
        src/server/QuicConnection.cpp
        src/server/QuicConnectionManager.cpp
        src/server/QuicHttp3Session.cpp
    )
    add_compile_definitions(ORBIT_ENABLE_HTTP3=1)
endif()

if(ORBIT_ENABLE_REDIS)
    list(APPEND SERVER_SOURCES src/database/RedisClient.cpp)
    add_compile_definitions(ORBIT_ENABLE_REDIS=1)
endif()
if(ORBIT_ENABLE_POSTGRES)
    list(APPEND SERVER_SOURCES src/database/PostgresClient.cpp)
    add_compile_definitions(ORBIT_ENABLE_POSTGRES=1)
endif()
if(ORBIT_ENABLE_MARIADB)
    list(APPEND SERVER_SOURCES src/database/MysqlClient.cpp)
    add_compile_definitions(ORBIT_ENABLE_MARIADB=1)
endif()
if(ORBIT_ENABLE_MONGODB)
    list(APPEND SERVER_SOURCES src/database/MongoClient.cpp)
    add_compile_definitions(ORBIT_ENABLE_MONGODB=1)
endif()

add_library(server_core ${SERVER_SOURCES})

# If building as a shared library, define exports
if(BUILD_SHARED_LIBS)
    target_compile_definitions(server_core PRIVATE ORBIT_EXPORTS)
    set_target_properties(server_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endif()

add_library(OrbitFramework::core ALIAS server_core)

target_link_libraries(server_core
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        PkgConfig::NGHTTP2
        inja
        CURL::libcurl
)

if(ORBIT_ENABLE_POSTGRES)
    message(STATUS "PostgreSQL_FOUND = ${PostgreSQL_FOUND}")
    message(STATUS "PostgreSQL_LIBRARIES = ${PostgreSQL_LIBRARIES}")
    message(STATUS "PostgreSQL_INCLUDE_DIRS = ${PostgreSQL_INCLUDE_DIRS}")
    if(TARGET PostgreSQL::PostgreSQL)
        message(STATUS "TARGET PostgreSQL::PostgreSQL is defined!")
        target_link_libraries(server_core PUBLIC PostgreSQL::PostgreSQL)
    else()
        message(STATUS "TARGET PostgreSQL::PostgreSQL is NOT defined!")
        target_link_libraries(server_core PUBLIC ${PostgreSQL_LIBRARIES})
        target_include_directories(server_core PUBLIC $<BUILD_INTERFACE:${PostgreSQL_INCLUDE_DIRS}>)
    endif()
endif()
if(ORBIT_ENABLE_MARIADB)
    target_link_libraries(server_core PUBLIC ${MARIADB_LIBRARIES})
    target_link_directories(server_core PUBLIC $<BUILD_INTERFACE:${MARIADB_LIBRARY_DIRS}>)
    target_include_directories(server_core PUBLIC $<BUILD_INTERFACE:${MARIADB_INCLUDE_DIRS}>)
endif()
if(ORBIT_ENABLE_MONGODB)
    target_link_libraries(server_core PUBLIC ${MONGOC_LIBRARIES})
    target_link_directories(server_core PUBLIC $<BUILD_INTERFACE:${MONGOC_LIBRARY_DIRS}>)
    target_include_directories(server_core PUBLIC $<BUILD_INTERFACE:${MONGOC_INCLUDE_DIRS}>)
endif()
if(ORBIT_ENABLE_REDIS)
    target_link_libraries(server_core PUBLIC ${HIREDIS_LIBRARIES})
    target_link_directories(server_core PUBLIC $<BUILD_INTERFACE:${HIREDIS_LIBRARY_DIRS}>)
    target_include_directories(server_core PUBLIC $<BUILD_INTERFACE:${HIREDIS_INCLUDE_DIRS}>)
endif()
if(ORBIT_ENABLE_HTTP3)
    target_link_libraries(server_core PUBLIC ${NGTCP2_LIBRARIES} ${NGTCP2_CRYPTO_LIBRARIES} ${NGHTTP3_LIBRARIES})
    target_link_directories(server_core PUBLIC 
        $<BUILD_INTERFACE:${NGTCP2_LIBRARY_DIRS}>
        $<BUILD_INTERFACE:${NGTCP2_CRYPTO_LIBRARY_DIRS}>
        $<BUILD_INTERFACE:${NGHTTP3_LIBRARY_DIRS}>
    )
    target_include_directories(server_core PUBLIC 
        $<BUILD_INTERFACE:${NGTCP2_INCLUDE_DIRS}>
        $<BUILD_INTERFACE:${NGTCP2_CRYPTO_INCLUDE_DIRS}>
        $<BUILD_INTERFACE:${NGHTTP3_INCLUDE_DIRS}>
    )
endif()

if(ORBIT_ENABLE_GRPC)
    find_package(gRPC CONFIG REQUIRED)
    target_link_libraries(server_core PUBLIC gRPC::grpc++)
    add_compile_definitions(ORBIT_ENABLE_GRPC=1)
endif()

if(WIN32)
    target_link_libraries(server_core PUBLIC ws2_32 mswsock)
elseif(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
    target_link_libraries(server_core PUBLIC PkgConfig::URING)
endif()

target_include_directories(server_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    # Install Rules for the Framework
    include(GNUInstallDirs)

    install(TARGETS server_core
        EXPORT OrbitFrameworkTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )

    install(EXPORT OrbitFrameworkTargets
        FILE OrbitFrameworkTargets.cmake
        NAMESPACE OrbitFramework::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OrbitFramework
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/OrbitFrameworkConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OrbitFrameworkConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OrbitFramework
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OrbitFrameworkConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/OrbitFrameworkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/OrbitFrameworkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OrbitFramework
    )

    # Doxygen Configuration
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM
        )
    endif()

    # Examples
    option(ORBIT_BUILD_EXAMPLES "Build example applications" ON)
    if(ORBIT_BUILD_EXAMPLES)
        add_executable(basic_server examples/basic_server.cpp)
        target_link_libraries(basic_server PRIVATE server_core)

        add_executable(rest_api examples/rest_api.cpp)
        target_link_libraries(rest_api PRIVATE server_core)

        add_executable(chat_server examples/chat_server.cpp)
        target_link_libraries(chat_server PRIVATE server_core)

        add_executable(graphql_server examples/graphql_server.cpp)
        target_link_libraries(graphql_server PRIVATE server_core)

        add_executable(database_server examples/database_server.cpp)
        target_link_libraries(database_server PRIVATE server_core)
    endif()

    # Testing Setup (GoogleTest via FetchContent)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(http_server_tests 
        tests/test_parser.cpp
        tests/test_router.cpp
        tests/test_threadpool.cpp
        tests/test_websocket.cpp
        tests/test_ssr.cpp
    )
    target_link_libraries(http_server_tests PRIVATE gtest_main server_core)
    include(GoogleTest)
    gtest_discover_tests(http_server_tests)

    # Fuzz Testing
    option(ENABLE_FUZZING "Build fuzz testing executables" OFF)
    if(ENABLE_FUZZING)
        add_executable(fuzz_http_parser tests/fuzz_http_parser.cpp)
        target_link_libraries(fuzz_http_parser PRIVATE server_core)
        # Enable libFuzzer and AddressSanitizer
        target_compile_options(fuzz_http_parser PRIVATE -fsanitize=fuzzer,address -g -O1)
        target_link_options(fuzz_http_parser PRIVATE -fsanitize=fuzzer,address)
    endif()
endif()

# --- Packaging with CPack ---
set(CPACK_PACKAGE_NAME "OrbitFramework")
set(CPACK_PACKAGE_VENDOR "Orbit Framework Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A blazing fast C++20 HTTP/3 web framework")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# For Linux, build DEB and RPM by default if tools are present
set(CPACK_GENERATOR "TGZ;ZIP")
if(UNIX AND NOT APPLE)
    list(APPEND CPACK_GENERATOR "DEB" "RPM")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3, zlib1g")
endif()

# For macOS
if(APPLE)
    list(APPEND CPACK_GENERATOR "DragNDrop")
endif()

# For Windows
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS")
endif()

include(CPack)
