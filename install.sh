#!/usr/bin/env bash
#
# Orbit Framework installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.sh | bash
#
# Environment variables:
#   ORBIT_VERSION  Release tag to install (default: the latest release).
#                  Set to "main" to install the unreleased development branch.
#   ORBIT_PREFIX   Installation prefix (default: /usr/local). sudo is only used
#                  when this directory is not writable, so e.g.
#                  ORBIT_PREFIX="$HOME/.local" installs without root.
#   ORBIT_JOBS     Parallel build jobs (default: limited by free memory, since
#                  each C++ compile job needs roughly 2 GB).
set -euo pipefail

REPO="varuns2903/orbit-framework"
PREFIX="${ORBIT_PREFIX:-/usr/local}"

echo "🚀 Welcome to the Orbit Framework Installer!"

# --- Platform -----------------------------------------------------------------
case "$(uname -s)" in
    Linux*)  MACHINE=Linux ;;
    Darwin*) MACHINE=Mac ;;
    CYGWIN*|MINGW*|MSYS*)
        echo "❌ This script is for Linux/macOS. On Windows, use install.ps1." >&2
        exit 1 ;;
    *)
        echo "❌ Unsupported platform: $(uname -s)" >&2
        exit 1 ;;
esac

for tool in git cmake curl; do
    if ! command -v "$tool" > /dev/null; then
        echo "❌ '$tool' is required but was not found." >&2
        exit 1
    fi
done

# --- Version ------------------------------------------------------------------
# Install a published release, not whatever happens to be on main right now.
VERSION="${ORBIT_VERSION:-}"
if [ -z "$VERSION" ]; then
    VERSION="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
        | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -n 1)"
    if [ -z "$VERSION" ]; then
        echo "❌ Could not determine the latest release. Set ORBIT_VERSION (e.g. ORBIT_VERSION=v1.6.0)." >&2
        exit 1
    fi
fi
if [ "$VERSION" = "main" ]; then
    echo "⚠️  Installing the development branch (main), not a release."
else
    echo "📌 Installing Orbit ${VERSION}"
fi

# --- Privileges ---------------------------------------------------------------
SUDO=""
mkdir -p "$PREFIX" 2> /dev/null || true
if [ ! -w "$PREFIX" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        if ! command -v sudo > /dev/null; then
            echo "❌ $PREFIX is not writable and sudo is not available. Set ORBIT_PREFIX to a writable directory." >&2
            exit 1
        fi
        SUDO="sudo"
        echo "🔐 $PREFIX is not writable; sudo will be used for the install step only."
    fi
fi

# --- Parallelism --------------------------------------------------------------
if [ -n "${ORBIT_JOBS:-}" ]; then
    JOBS="$ORBIT_JOBS"
else
    if command -v nproc > /dev/null; then
        CORES="$(nproc)"
    else
        CORES="$(sysctl -n hw.ncpu 2> /dev/null || echo 2)"
    fi
    if [ "$MACHINE" = "Linux" ] && [ -r /proc/meminfo ]; then
        MEM_GB="$(awk '/MemAvailable/ {printf "%d", $2 / 1048576}' /proc/meminfo)"
    else
        MEM_GB="$(( $(sysctl -n hw.memsize 2> /dev/null || echo 4294967296) / 1073741824 ))"
    fi
    MEM_JOBS=$(( MEM_GB / 2 ))
    JOBS=$(( CORES < MEM_JOBS ? CORES : MEM_JOBS ))
    if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
fi
echo "🧮 Building with ${JOBS} parallel job(s) (override with ORBIT_JOBS)."

# --- Workspace ----------------------------------------------------------------
TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT
cd "$TMP_DIR"

echo "📥 Downloading Orbit ${VERSION}..."
git clone --quiet --depth 1 --branch "$VERSION" "https://github.com/${REPO}.git" orbit-framework
cd orbit-framework

# --- Dependencies -------------------------------------------------------------
# Pin vcpkg to the baseline the release was tested with, so dependency versions
# are reproducible instead of tracking vcpkg's moving tip.
BASELINE="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' vcpkg.json)"
echo "📦 Setting up vcpkg (baseline ${BASELINE:-latest})..."
if [ -n "$BASELINE" ]; then
    mkdir vcpkg
    git -C vcpkg init --quiet
    git -C vcpkg remote add origin https://github.com/microsoft/vcpkg.git
    git -C vcpkg fetch --quiet --depth 1 origin "$BASELINE"
    git -C vcpkg checkout --quiet FETCH_HEAD
else
    git clone --quiet --depth 1 https://github.com/microsoft/vcpkg.git
fi
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Reuse already-built dependency binaries across installer runs instead of
# recompiling OpenSSL/curl/mongo-c-driver/etc. from source every time.
CACHE_DIR="${VCPKG_DEFAULT_BINARY_CACHE:-$HOME/.cache/vcpkg-binary-cache}"
mkdir -p "$CACHE_DIR"
export VCPKG_BINARY_SOURCES="clear;files,${CACHE_DIR},readwrite"

# --- Build --------------------------------------------------------------------
echo "🔨 Building Orbit Framework (this may take a while)..."
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DORBIT_BUILD_TESTS=OFF \
    -DORBIT_BUILD_EXAMPLES=OFF \
    .
cmake --build build -j "$JOBS"

# --- Install ------------------------------------------------------------------
echo "💾 Installing the framework to ${PREFIX}..."
$SUDO cmake --install build

echo "🛠️  Installing the orbit CLI to ${PREFIX}/bin..."
$SUDO mkdir -p "${PREFIX}/bin"
$SUDO install -m 0755 tools/cli/orbit "${PREFIX}/bin/orbit"

echo ""
echo "✅ Orbit ${VERSION} installed to ${PREFIX}."
case ":${PATH}:" in
    *":${PREFIX}/bin:"*) ;;
    *) echo "ℹ️  Add ${PREFIX}/bin to your PATH to use the 'orbit' command." ;;
esac
echo "Create a project with:"
echo "    orbit new my_project"
echo "    cd my_project"
echo "    orbit build"
echo "    orbit run"
