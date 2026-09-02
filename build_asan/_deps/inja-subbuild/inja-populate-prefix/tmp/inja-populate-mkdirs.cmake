# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/data/Projects/orbit-framework/build_asan/_deps/inja-src")
  file(MAKE_DIRECTORY "/data/Projects/orbit-framework/build_asan/_deps/inja-src")
endif()
file(MAKE_DIRECTORY
  "/data/Projects/orbit-framework/build_asan/_deps/inja-build"
  "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix"
  "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/tmp"
  "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/src/inja-populate-stamp"
  "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/src"
  "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/src/inja-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/src/inja-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/data/Projects/orbit-framework/build_asan/_deps/inja-subbuild/inja-populate-prefix/src/inja-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
