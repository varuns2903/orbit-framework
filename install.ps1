Write-Host "🚀 Welcome to the Orbit Framework Installer for Windows!" -ForegroundColor Cyan
Write-Host "This script will download, build, and install Orbit and its CLI tool."

# Requires Administrator for copying to Program Files and system PATH
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-Not $isAdmin) {
    Write-Host "⚠️ Warning: You are not running as Administrator. Installation to Program Files might fail." -ForegroundColor Yellow
    Write-Host "Restart PowerShell as Administrator if you encounter permission errors."
}

# 1. Clone the repository
$TmpDir = Join-Path $env:TEMP "orbit_installer_$(Get-Random)"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
Set-Location $TmpDir

Write-Host "📥 Cloning Orbit Framework..." -ForegroundColor Green
git clone --depth 1 https://github.com/varuns2903/orbit-framework.git
Set-Location orbit-framework

# 2. Bootstrap vcpkg
Write-Host "📦 Setting up vcpkg..." -ForegroundColor Green
git clone --depth 1 https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat -disableMetrics

# 3. Build the framework
Write-Host "🔨 Building Orbit Framework (this may take a few minutes)..." -ForegroundColor Green
$Cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
if (-not $Cores) { $Cores = 4 }

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake .
cmake --build build -j $Cores --config Release

# 4. Install the framework
Write-Host "💾 Installing framework libraries to system..." -ForegroundColor Green
cmake --install build --config Release

# 5. Install the CLI
Write-Host "🛠️ Installing orbit-cli..." -ForegroundColor Green
$InstallDir = "C:\Program Files\OrbitFramework\bin"
if (-Not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

# Copy python script and create a batch wrapper so it can be executed from cmd/powershell
Copy-Item tools\cli\orbit "$InstallDir\orbit.py" -Force
$BatContent = "@echo off`npython `"%~dp0orbit.py`" %*"
Set-Content -Path "$InstallDir\orbit.bat" -Value $BatContent

# Add to Machine PATH if not exists
$Path = [Environment]::GetEnvironmentVariable("PATH", "Machine")
if ($Path -notmatch [regex]::Escape($InstallDir)) {
    Write-Host "Adding $InstallDir to System PATH..."
    [Environment]::SetEnvironmentVariable("PATH", $Path + ";$InstallDir", "Machine")
    Write-Host "⚠️ You may need to restart your terminal for the 'orbit' command to be recognized." -ForegroundColor Yellow
}

# Cleanup
Set-Location $env:USERPROFILE
Remove-Item -Recurse -Force $TmpDir

Write-Host ""
Write-Host "✅ Installation Complete!" -ForegroundColor Green
Write-Host "You can now create a new project by running:"
Write-Host "    orbit new my_project"
Write-Host "    cd my_project"
Write-Host "    orbit build"
Write-Host "    orbit run"
