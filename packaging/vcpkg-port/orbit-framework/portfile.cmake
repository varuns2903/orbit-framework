vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO varuns2903/orbit-framework
    REF "v${VERSION}"
    SHA512 e5da790fa60dddc343675a556109ecd52ef0c962c62567dec46e52fd27460eb8253d0d7f050477f2884c6cd9cb8645c85d83cdef9ffa5482e9371e16f19df7f2
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
