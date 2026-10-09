cmake_minimum_required(VERSION 3.20)

# vcpkg manifest features follow the ORBIT_ENABLE_* options, so a backend that
# is turned off is not downloaded and built. This has to happen before
# project(), where the vcpkg toolchain installs dependencies. An explicit
# -DVCPKG_MANIFEST_FEATURES=... from the user is left alone.
if(NOT DEFINED VCPKG_MANIFEST_FEATURES OR "${VCPKG_MANIFEST_FEATURES}" STREQUAL "${ORBIT_VCPKG_FEATURES_AUTO}")
    set(_orbit_features "")
    foreach(_orbit_entry IN ITEMS "POSTGRES;postgres;ON" "MARIADB;mariadb;ON" "MONGODB;mongodb;ON"
                                  "HTTP3;http3;OFF" "BROTLI;brotli;ON" "ZSTD;zstd;ON" "GRPC;grpc;OFF")
        list(GET _orbit_entry 0 _orbit_option)
        list(GET _orbit_entry 1 _orbit_feature)
        list(GET _orbit_entry 2 _orbit_default)
        if(DEFINED ORBIT_ENABLE_${_orbit_option})
            set(_orbit_enabled "${ORBIT_ENABLE_${_orbit_option}}")
        else()
            set(_orbit_enabled "${_orbit_default}")
        endif()
        if(_orbit_enabled)
            list(APPEND _orbit_features "${_orbit_feature}")
        endif()
    endforeach()
    # The HTTP/3 end-to-end tests need a libcurl that speaks HTTP/3; the
    # library itself does not.
    if((NOT DEFINED ORBIT_BUILD_TESTS OR ORBIT_BUILD_TESTS) AND "http3" IN_LIST _orbit_features)
        list(APPEND _orbit_features "http3-tests")
    endif()
    # Cache entries, not plain variables: the toolchain declares these as
    # cache variables, which (before policy CMP0126) would hide plain ones.
    set(VCPKG_MANIFEST_FEATURES "${_orbit_features}" CACHE STRING "vcpkg manifest features" FORCE)
    set(ORBIT_VCPKG_FEATURES_AUTO "${_orbit_features}" CACHE INTERNAL "")
    set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON CACHE BOOL "" FORCE)
endif()

project(OrbitFramework VERSION 2.0.0 LANGUAGES CXX)

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
# HTTP/3 is experimental (docs/http3.md), so it is opt-in.
option(ORBIT_ENABLE_HTTP3 "Enable experimental HTTP/3 and QUIC support" OFF)
option(ORBIT_ENABLE_BROTLI "Enable brotli response compression (middleware::compress)" ON)
option(ORBIT_ENABLE_ZSTD "Enable zstd response compression (middleware::compress)" ON)
option(ORBIT_ENABLE_GRPC "Enable gRPC support" OFF)
# Sanitizers are for developing Orbit itself. When Orbit is a dependency
# (FetchContent / add_subdirectory) they default to off: instrumenting
# server_core forces the sanitizer runtime on the application, which is the
# application's choice to make.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_orbit_sanitizers_default ON)
else()
    set(_orbit_sanitizers_default OFF)
endif()
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers (Debug builds)" ${_orbit_sanitizers_default})
option(ORBIT_ENABLE_TSAN "Build with ThreadSanitizer (Debug only; replaces ASan/UBSan)" OFF)
option(ORBIT_ENABLE_COVERAGE "Enable code coverage generation" OFF)

if(ORBIT_ENABLE_COVERAGE)
    # Atomic counters: the tests run many threads, and non-atomic updates
    # leave impossible counts (e.g. "branch taken -1") that gcovr rejects.
    add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
    add_link_options(--coverage -fprofile-update=atomic)
endif()

# Optional sanitizers for debug builds to catch memory leaks and UB.
# ThreadSanitizer cannot be combined with ASan, so it takes precedence.
# add_compile_options()/add_link_options() reach only targets in this
# directory tree; ORBIT_SANITIZER_LINK_FLAGS is also put on server_core's
# INTERFACE below, so an application that links Orbit links the runtime too.
set(ORBIT_SANITIZER_LINK_FLAGS "")
if(ORBIT_ENABLE_TSAN AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    set(ORBIT_SANITIZER_LINK_FLAGS -fsanitize=thread)
elseif(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    set(ORBIT_SANITIZER_LINK_FLAGS -fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
if(ORBIT_ENABLE_POSTGRES)
    find_package(PostgreSQL REQUIRED)
endif()
find_package(CURL REQUIRED)
# HTTP/1.1 request parsing (src/http/Http1Parser.cpp). vcpkg installs only the
# static library; system packages may provide only the shared one.
find_package(llhttp CONFIG REQUIRED)
if(TARGET llhttp::llhttp_static)
    set(ORBIT_LLHTTP_TARGET llhttp::llhttp_static)
else()
    set(ORBIT_LLHTTP_TARGET llhttp::llhttp_shared)
endif()
find_package(PkgConfig REQUIRED)

if(ORBIT_ENABLE_MARIADB)
    find_package(unofficial-libmariadb CONFIG QUIET)
    if(unofficial-libmariadb_FOUND)
        set(MARIADB_LIBRARIES unofficial::libmariadb)
    else()
        pkg_search_module(MARIADB REQUIRED IMPORTED_TARGET libmariadb mariadb)
        set(MARIADB_LIBRARIES PkgConfig::MARIADB)
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
        pkg_search_module(MONGOC REQUIRED IMPORTED_TARGET libmongoc-1.0 mongoc-1.0 mongoc2)
        set(MONGOC_LIBRARIES PkgConfig::MONGOC)
    endif()
endif()

pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)
if(ORBIT_ENABLE_BROTLI)
    pkg_check_modules(BROTLIENC REQUIRED IMPORTED_TARGET libbrotlienc)
endif()
if(ORBIT_ENABLE_ZSTD)
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
endif()

if(ORBIT_ENABLE_HTTP3)
    pkg_check_modules(NGTCP2 REQUIRED IMPORTED_TARGET libngtcp2)
    pkg_search_module(NGTCP2_CRYPTO REQUIRED IMPORTED_TARGET libngtcp2_crypto_openssl libngtcp2_crypto_ossl libngtcp2_crypto_quictls)
    if(NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_ossl" OR NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_openssl")
        list(APPEND ORBIT_PUBLIC_DEFINITIONS USE_NGTCP2_CRYPTO_OSSL=1)
    elseif(NGTCP2_CRYPTO_MODULE_NAME STREQUAL "libngtcp2_crypto_quictls")
        list(APPEND ORBIT_PUBLIC_DEFINITIONS USE_NGTCP2_CRYPTO_QUICTLS=1)
    endif()
    pkg_check_modules(NGHTTP3 REQUIRED IMPORTED_TARGET libnghttp3)
endif()

# inja (templates in src/http/HttpResponse.cpp) is fetched at configure time by
# default. Package managers build without network access, so they set
# ORBIT_USE_SYSTEM_INJA to use an installed inja instead (the vcpkg port does).
option(ORBIT_USE_SYSTEM_INJA "Use an installed inja (find_package) instead of fetching it" OFF)
if(ORBIT_USE_SYSTEM_INJA)
    find_package(inja CONFIG REQUIRED)
    set(ORBIT_INJA_TARGET pantor::inja)
else()
    include(FetchContent)
    FetchContent_Declare(
      inja
      GIT_REPOSITORY https://github.com/pantor/inja.git
      GIT_TAG        v3.4.0
    )
    set(INJA_BUILD_TESTS OFF CACHE BOOL "Disable inja tests" FORCE)
    set(BUILD_BENCHMARK OFF CACHE BOOL "Disable inja benchmark" FORCE)
    FetchContent_MakeAvailable(inja)
    # Third-party headers: Orbit's -Wshadow/-Wconversion must not fire inside
    # them (about 100 warnings in every build that fetches Orbit, #186).
    get_target_property(_orbit_inja_includes inja INTERFACE_INCLUDE_DIRECTORIES)
    if(_orbit_inja_includes)
        set_target_properties(inja PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_orbit_inja_includes}")
    endif()
    set(ORBIT_INJA_TARGET inja)
endif()


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
    src/config/ConfigFile.cpp
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
    src/concurrency/Task.cpp
    src/http/HttpParser.cpp
    src/http/Http1Parser.cpp
    src/http/HttpResponse.cpp
    src/http/ResponseWriter.cpp
    src/http/MultipartForm.cpp
    src/http/MultipartStreamParser.cpp
    src/http/MultipartUpload.cpp
    src/http/Client.cpp
    src/http/WebSocketConnection.cpp
    src/http/Http2Session.cpp
    src/routing/Router.cpp
    src/server/App.cpp
    src/server/Scheduler.cpp
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
    src/middleware/Observability.cpp
    src/middleware/Csrf.cpp
    src/middleware/OAuth2.cpp
    src/middleware/SecurityHeaders.cpp
    src/middleware/TrustedProxies.cpp
    src/openapi/OpenApi.cpp
    src/testing/Client.cpp
)

if(ORBIT_ENABLE_HTTP3)
    list(APPEND SERVER_SOURCES 
        src/server/QuicConnection.cpp
        src/server/QuicConnectionManager.cpp
        src/server/QuicHttp3Session.cpp
    )
    list(APPEND ORBIT_PUBLIC_DEFINITIONS ORBIT_ENABLE_HTTP3=1)
endif()

if(ORBIT_ENABLE_REDIS)
    list(APPEND SERVER_SOURCES src/database/RedisClient.cpp)
    list(APPEND ORBIT_PUBLIC_DEFINITIONS ORBIT_ENABLE_REDIS=1)
endif()
if(ORBIT_ENABLE_POSTGRES)
    list(APPEND SERVER_SOURCES src/database/PostgresClient.cpp)
    list(APPEND ORBIT_PUBLIC_DEFINITIONS ORBIT_ENABLE_POSTGRES=1)
endif()
if(ORBIT_ENABLE_MARIADB)
    list(APPEND SERVER_SOURCES src/database/MysqlClient.cpp)
    list(APPEND ORBIT_PUBLIC_DEFINITIONS ORBIT_ENABLE_MARIADB=1)
endif()
if(ORBIT_ENABLE_MONGODB)
    list(APPEND SERVER_SOURCES src/database/MongoClient.cpp)
    list(APPEND ORBIT_PUBLIC_DEFINITIONS ORBIT_ENABLE_MONGODB=1)
endif()

add_library(server_core ${SERVER_SOURCES})
if(ORBIT_SANITIZER_LINK_FLAGS)
    target_link_options(server_core INTERFACE ${ORBIT_SANITIZER_LINK_FLAGS})
endif()

# These macros gate conditional members in public headers — App.hpp declares
# quic_socket_ and quic_manager_ only under ORBIT_ENABLE_HTTP3, for instance.
# They were set with add_compile_definitions(), which is directory-scoped and
# therefore never reached anything consuming Orbit. A consumer compiled
# App.hpp without ORBIT_ENABLE_HTTP3 while libserver_core.a was built with it,
# so sizeof(App) disagreed by 16 bytes and App's constructor wrote past the
# end of the caller's object. PUBLIC puts them on the target, so they
# propagate through the build tree, the installed export, and FetchContent
# alike, and the two sides always agree on layout.
target_compile_definitions(server_core PUBLIC ${ORBIT_PUBLIC_DEFINITIONS})
# Orbit's own code uses the orbit:: names only; the global aliases in
# legacy_namespaces.hpp are for applications written against 1.x.
target_compile_definitions(server_core PRIVATE ORBIT_NO_LEGACY_NAMESPACES)

# If building as a shared library, define exports
if(BUILD_SHARED_LIBS)
    target_compile_definitions(server_core PRIVATE ORBIT_EXPORTS)
    set_target_properties(server_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endif()

# Consumers see the same target name whether they arrive via find_package or
# FetchContent. EXPORT_NAME makes the installed export OrbitFramework::core to
# match the alias below; OrbitFramework::server_core stays as a compatibility
# spelling because earlier README examples used it.
set_target_properties(server_core PROPERTIES EXPORT_NAME core)
add_library(OrbitFramework::core ALIAS server_core)
add_library(OrbitFramework::server_core ALIAS server_core)

target_link_libraries(server_core
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        PkgConfig::NGHTTP2
        CURL::libcurl
    PRIVATE
        # llhttp appears in no public header (Http1Parser uses a pimpl); for a
        # static build it is still part of the export's link interface.
        ${ORBIT_LLHTTP_TARGET}
        # inja is header-only, used only by src/http/HttpResponse.cpp, and
        # appears in no public header. Linking it PUBLIC put pantor::inja in
        # the installed export, where consumers cannot resolve it; BUILD_INTERFACE
        # keeps it out of the exported link interface entirely.
        $<BUILD_INTERFACE:${ORBIT_INJA_TARGET}>
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
endif()
if(ORBIT_ENABLE_MONGODB)
    target_link_libraries(server_core PUBLIC ${MONGOC_LIBRARIES})
endif()
if(ORBIT_ENABLE_HTTP3)
    # Imported targets, not ${NGTCP2_LIBRARIES}: those are bare names such as
    # "nghttp3" whose -L directories never reached the installed export, so a
    # consumer of an installed Orbit failed to link. OrbitFrameworkConfig.cmake
    # recreates these targets (likewise PkgConfig::MARIADB and ::MONGOC above).
    target_link_libraries(server_core PUBLIC PkgConfig::NGTCP2 PkgConfig::NGTCP2_CRYPTO PkgConfig::NGHTTP3)
endif()

if(ORBIT_ENABLE_BROTLI)
    target_link_libraries(server_core PUBLIC PkgConfig::BROTLIENC)
    target_compile_definitions(server_core PRIVATE ORBIT_ENABLE_BROTLI=1)
endif()
if(ORBIT_ENABLE_ZSTD)
    target_link_libraries(server_core PUBLIC PkgConfig::ZSTD)
    target_compile_definitions(server_core PRIVATE ORBIT_ENABLE_ZSTD=1)
endif()

if(ORBIT_ENABLE_GRPC)
    find_package(gRPC CONFIG REQUIRED)
    target_link_libraries(server_core PUBLIC gRPC::grpc++)
    # Applied directly: this block runs after the target_compile_definitions
    # call above, so appending to ORBIT_PUBLIC_DEFINITIONS here would be lost.
    target_compile_definitions(server_core PUBLIC ORBIT_ENABLE_GRPC=1)
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
        # Each example is registered through this helper with the list of
        # subsystems its sources actually use. Examples were previously added
        # unconditionally, so disabling a subsystem still compiled an example
        # against headers whose implementation had been left out of
        # SERVER_SOURCES, and the build failed at link time with undefined
        # references. Configuring with a feature off should simply build fewer
        # examples.
        function(orbit_add_example name)
            foreach(feature IN LISTS ARGN)
                if(NOT ${feature})
                    message(STATUS "Orbit: skipping example ${name} (requires ${feature})")
                    return()
                endif()
            endforeach()
            add_executable(${name} examples/${name}.cpp)
            target_link_libraries(${name} PRIVATE server_core)
            target_compile_definitions(${name} PRIVATE ORBIT_NO_LEGACY_NAMESPACES)
        endfunction()

        # No optional subsystems: core HTTP, routing, WebSockets, templating.
        orbit_add_example(benchmark_server)
        # Counts heap allocations and serves the count at /__stats, for
        # benchmarks/probe.sh (allocations per request). Off by default: the
        # counting operator new adds an atomic increment to every allocation.
        option(ORBIT_BENCH_COUNT_ALLOCATIONS "benchmark_server: count heap allocations (for benchmarks/probe.sh)" OFF)
        if(ORBIT_BENCH_COUNT_ALLOCATIONS)
            target_compile_definitions(benchmark_server PRIVATE ORBIT_BENCH_COUNT_ALLOCATIONS=1)
        endif()
        orbit_add_example(chat_server)
        orbit_add_example(fastapi_server)
        orbit_add_example(graphql_server)
        orbit_add_example(websocket_event_server)
        orbit_add_example(websocket_echo_server)

        # Database-backed examples.
        orbit_add_example(database_server ORBIT_ENABLE_REDIS)
        orbit_add_example(rest_api ORBIT_ENABLE_POSTGRES)
        orbit_add_example(orm_server ORBIT_ENABLE_POSTGRES)
        orbit_add_example(migration_server ORBIT_ENABLE_POSTGRES)

        # basic_server is the kitchen-sink example and touches every client.
        orbit_add_example(basic_server
            ORBIT_ENABLE_REDIS
            ORBIT_ENABLE_POSTGRES
            ORBIT_ENABLE_MARIADB
            ORBIT_ENABLE_MONGODB
        )
    endif()

    # Testing Setup (GoogleTest via FetchContent)
    option(ORBIT_BUILD_TESTS "Build the GoogleTest suite (fetches googletest over the network)" ON)
    if(ORBIT_BUILD_TESTS)
        include(FetchContent)
        FetchContent_Declare(
          googletest
          GIT_REPOSITORY https://github.com/google/googletest.git
          GIT_TAG        v1.14.0
        )
        FetchContent_MakeAvailable(googletest)

        enable_testing()

        # Collected with CONFIGURE_DEPENDS so a new test file is picked up by the
        # next build instead of silently never being compiled, which is how
        # test_http2.cpp and test_websocket_conn.cpp sat outside the build.
        file(GLOB ORBIT_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/*.cpp"
        )
        if(NOT ORBIT_TEST_SOURCES)
            message(FATAL_ERROR "ORBIT_BUILD_TESTS is ON but no test sources were found under tests/")
        endif()
        list(LENGTH ORBIT_TEST_SOURCES ORBIT_TEST_SOURCE_COUNT)
        message(STATUS "Orbit: building ${ORBIT_TEST_SOURCE_COUNT} test translation units")

        add_executable(http_server_tests ${ORBIT_TEST_SOURCES})
        target_link_libraries(http_server_tests PRIVATE gtest_main server_core CURL::libcurl)
        # The tests use the orbit:: names; test_legacy_namespaces.cpp turns
        # the 1.x aliases back on for itself.
        target_compile_definitions(http_server_tests PRIVATE ORBIT_NO_LEGACY_NAMESPACES)
        # The compression tests decode what the middleware produced.
        if(ORBIT_ENABLE_BROTLI)
            pkg_check_modules(BROTLIDEC REQUIRED IMPORTED_TARGET libbrotlidec)
            target_link_libraries(http_server_tests PRIVATE PkgConfig::BROTLIDEC)
            target_compile_definitions(http_server_tests PRIVATE ORBIT_ENABLE_BROTLI=1)
        endif()
        if(ORBIT_ENABLE_ZSTD)
            target_compile_definitions(http_server_tests PRIVATE ORBIT_ENABLE_ZSTD=1)
        endif()
        include(GoogleTest)
        # Listing ~650 tests can take longer than the default 5 s on a busy
        # CI runner (#151). PRE_TEST lists them when ctest runs rather than as
        # a post-build step, so a slow listing cannot fail the build itself.
        gtest_discover_tests(http_server_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST)
    endif()

    # Fuzz Testing
    option(ENABLE_FUZZING "Build fuzz testing executables" OFF)
    if(ENABLE_FUZZING AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # libFuzzer is coverage-guided: the code under test (in server_core)
        # must carry the coverage instrumentation, not just the harness, or
        # the fuzzer cannot tell which inputs reach new code.
        target_compile_options(server_core PRIVATE -fsanitize=fuzzer-no-link,address -g -O1)

        foreach(fuzz_target IN ITEMS fuzz_http1_parser fuzz_websocket fuzz_multipart fuzz_http2)
            add_executable(${fuzz_target} tests/fuzz/${fuzz_target}.cpp)
            target_link_libraries(${fuzz_target} PRIVATE server_core)
            target_compile_definitions(${fuzz_target} PRIVATE ORBIT_NO_LEGACY_NAMESPACES)
            # Enable libFuzzer and AddressSanitizer
            target_compile_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address -g -O1)
            target_link_options(${fuzz_target} PRIVATE -fsanitize=fuzzer,address)
        endforeach()
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
    if(ORBIT_ENABLE_BROTLI)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libbrotli1")
    endif()
    if(ORBIT_ENABLE_ZSTD)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libzstd1")
    endif()
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
