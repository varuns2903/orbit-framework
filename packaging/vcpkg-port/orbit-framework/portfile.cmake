vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO varuns2903/orbit-framework
    REF "v${VERSION}"
    SHA512 1f3fbcf6b02939431a0d4e4bb6cea48db37d82014f5ab919bb58bbd35f3557bc56e89a386202b9bb831837f230fec69c469010318d71f770e122758b2703919e
    HEAD_REF main
    PATCHES
        # Upstream after v2.0.0; drop with the next release: ORBIT_USE_SYSTEM_INJA
        # (no FetchContent during the build), gRPC in the package config, and
        # HTTP/3 libraries exported as imported targets so consumers can link.
        fix-packaging.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        postgres    ORBIT_ENABLE_POSTGRES
        mariadb     ORBIT_ENABLE_MARIADB
        mongodb     ORBIT_ENABLE_MONGODB
        http3       ORBIT_ENABLE_HTTP3
        brotli      ORBIT_ENABLE_BROTLI
        zstd        ORBIT_ENABLE_ZSTD
        grpc        ORBIT_ENABLE_GRPC
)

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        ${FEATURE_OPTIONS}
        # The Redis client speaks RESP itself and needs no library.
        -DORBIT_ENABLE_REDIS=ON
        -DORBIT_USE_SYSTEM_INJA=ON
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DENABLE_SANITIZERS=OFF
        -DORBIT_ENABLE_COVERAGE=OFF
        -DORBIT_BUILD_EXAMPLES=OFF
        -DORBIT_BUILD_TESTS=OFF
        -DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME OrbitFramework
    CONFIG_PATH lib/cmake/OrbitFramework
)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
