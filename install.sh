#!/usr/bin/env bash
set -e

echo "🚀 Welcome to the Orbit Framework Installer!"
echo "This script will download, build, and install Orbit and its CLI tool."

# Check for root/sudo
if [ "$EUID" -ne 0 ]; then 
  SUDO="sudo"
  echo "⚠️ This script requires sudo to install to /usr/local"
else
  SUDO=""
fi

# 1. Clone the repository
TMP_DIR=$(mktemp -d)
cd $TMP_DIR
echo "📥 Cloning Orbit Framework..."
git clone --depth 1 https://github.com/varuns2903/orbit-framework.git
cd orbit-framework

# 2. Bootstrap vcpkg
echo "📦 Setting up vcpkg..."
git clone --depth 1 https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# 3. Build the framework
echo "🔨 Building Orbit Framework (this may take a few minutes)..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake .
cmake --build build -j$(nproc)

# 4. Install the framework
echo "💾 Installing framework libraries to system..."
$SUDO cmake --install build

# 5. Install the CLI
echo "🛠️ Installing orbit-cli to /usr/local/bin..."
$SUDO cp tools/cli/orbit /usr/local/bin/orbit
$SUDO chmod +x /usr/local/bin/orbit

# Cleanup
cd ~
rm -rf $TMP_DIR

echo ""
echo "✅ Installation Complete!"
echo "You can now create a new project by running:"
echo "    orbit new my_project"
echo "    cd my_project"
echo "    orbit build"
echo "    orbit run"
