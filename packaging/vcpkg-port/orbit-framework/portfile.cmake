vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO varuns2903/orbit-framework
    REF "v${VERSION}"
    SHA512 55f5a93b13992739b24c72f0513e8b5e6dd448cf16d86ccb9474c0dbfdc543f6b7b58fff39bbab6455e19ec86a945f90cd67b102ed327ef5d0d47ba81a0e9e68
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
