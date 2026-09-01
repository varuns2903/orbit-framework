from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps

class OrbitFrameworkRecipe(ConanFile):
    name = "orbit-framework"
    version = "0.1.0"
    package_type = "library"

    # Metadata
    license = "MIT"
    author = "Orbit Framework Contributors"
    url = "https://github.com/varuns2903/orbit-framework"
    description = "A blazing fast, asynchronous, and middleware-driven C++20 HTTP/3 web framework"
    topics = ("http3", "framework", "cpp20", "io_uring", "coroutine", "quic")

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
