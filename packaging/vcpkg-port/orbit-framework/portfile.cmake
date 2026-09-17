vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO varuns2903/orbit-framework
    REF "v${VERSION}"
    SHA512 9c3b780fce8b5d558ff1886c76b2f3f7e255742a7aff8f875f1f0670a6290c4b1356b0d3ded00891a720f923371c561a8be34e8c55ff689871f4a28dc0c0dc49
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
