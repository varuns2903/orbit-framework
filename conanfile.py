import os
import re

from conan import ConanFile
from conan.errors import ConanException
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps


class OrbitFrameworkRecipe(ConanFile):
    name = "orbit-framework"
    package_type = "library"

    def set_version(self):
        # Read the version straight out of CMakeLists.txt rather than repeating
        # it here. The hardcoded value had drifted to 0.1.0 while the project
        # shipped 1.4.0, so every `conan create` produced a package claiming a
        # version three releases old.
        cmakelists = os.path.join(self.recipe_folder, "CMakeLists.txt")
        with open(cmakelists, "r", encoding="utf-8") as handle:
            match = re.search(r"project\s*\([^)]*VERSION\s+([0-9]+(?:\.[0-9]+)*)", handle.read())
        if not match:
            raise ConanException("could not read project VERSION from CMakeLists.txt")
        self.version = match.group(1)

    # Metadata
    license = "MIT"
    author = "Orbit Framework Contributors"
    url = "https://github.com/varuns2903/orbit-framework"
    description = "A blazing fast, asynchronous, and middleware-driven C++20 HTTP/3 web framework"
    topics = ("http3", "framework", "cpp20", "io_uring", "coroutine", "quic")

    # set_version reads CMakeLists.txt, so it has to be exported with the
    # recipe itself, not only with the sources.
    exports = "CMakeLists.txt"
    exports_sources = (
        "CMakeLists.txt",
        "LICENSE",
        "OrbitFrameworkConfig.cmake.in",
        "Doxyfile.in",
        "include/*",
        "src/*",
    )

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    def requirements(self):
        self.requires("openssl/3.2.0")
        self.requires("zlib/1.3")
        self.requires("libpq/15.4")
        self.requires("libcurl/8.5.0")
        self.requires("mariadb-connector-c/3.3.3")
        self.requires("mongo-c-driver/1.25.0")
        self.requires("hiredis/1.1.0")
        self.requires("nghttp2/1.58.0")
        # Note: ngtcp2 & nghttp3 might require manual recipes or custom conan remotes
        # self.requires("ngtcp2/1.1.0")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        # Tests pull GoogleTest in with FetchContent at configure time, and
        # examples are not part of the installed package. Neither belongs in a
        # package build, which may well run without network access.
        tc.cache_variables["ORBIT_BUILD_TESTS"] = False
        tc.cache_variables["ORBIT_BUILD_EXAMPLES"] = False
        tc.cache_variables["ENABLE_SANITIZERS"] = False
        tc.cache_variables["ORBIT_ENABLE_COVERAGE"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["server_core"]
        self.cpp_info.set_property("cmake_target_name", "OrbitFramework::core")
