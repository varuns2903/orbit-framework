set(INJA_VERSION "3.4.0")

set(INJA_PACKAGE_USE_EMBEDDED_JSON "OFF")

include(CMakeFindDependencyMacro)

if(NOT INJA_PACKAGE_USE_EMBEDDED_JSON)
    find_dependency(nlohmann_json REQUIRED)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/injaTargets.cmake")
