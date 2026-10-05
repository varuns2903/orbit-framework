vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO varuns2903/orbit-framework
    REF "v${VERSION}"
    SHA512 dd1254c07d948febac267c0bd61d823134ee3b3a52036adaaa24a332b0e32f89646c966d33e18519584975f3d12f9afbc29d1d2d5e868672ae3e2fe1d9f45f5d
    HEAD_REF main
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        grpc ORBIT_ENABLE_GRPC
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DORBIT_ENABLE_REDIS=ON
        -DORBIT_ENABLE_POSTGRES=ON
        -DORBIT_ENABLE_MARIADB=ON
        -DORBIT_ENABLE_MONGODB=ON
        -DORBIT_ENABLE_HTTP3=ON
        -DORBIT_ENABLE_BROTLI=ON
        -DORBIT_ENABLE_ZSTD=ON
        -DENABLE_SANITIZERS=OFF
        -DORBIT_ENABLE_COVERAGE=OFF
        -DORBIT_BUILD_EXAMPLES=OFF
        -DORBIT_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME OrbitFramework
    CONFIG_PATH lib/cmake/OrbitFramework
)

vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
