# Orbit Framework installer for Windows.
#
#   iwr -useb https://raw.githubusercontent.com/varuns2903/orbit-framework/main/install.ps1 | iex
#
# Environment variables:
#   ORBIT_VERSION  Release tag to install (default: the latest release).
#                  Set to "main" to install the unreleased development branch.
#   ORBIT_PREFIX   Installation prefix (default: C:\Program Files\OrbitFramework).
#   ORBIT_JOBS     Parallel build jobs (default: limited by free memory, since
#                  each C++ compile job needs roughly 2 GB).

$ErrorActionPreference = "Stop"
$Repo = "varuns2903/orbit-framework"

# Native commands do not throw on failure; stop on a non-zero exit code.
function Invoke-Checked {
    param([string]$Description, [scriptblock]$Command)
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

Write-Host "🚀 Welcome to the Orbit Framework Installer for Windows!" -ForegroundColor Cyan

foreach ($tool in @("git", "cmake")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "'$tool' is required but was not found on PATH."
    }
}

# --- Version ------------------------------------------------------------------
# Install a published release, not whatever happens to be on main right now.
$Version = $env:ORBIT_VERSION
if (-not $Version) {
    $Version = (Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$Repo/releases/latest").tag_name
    if (-not $Version) {
        throw "Could not determine the latest release. Set ORBIT_VERSION (e.g. `$env:ORBIT_VERSION = 'v1.5.1')."
    }
}
if ($Version -eq "main") {
    Write-Host "⚠️ Installing the development branch (main), not a release." -ForegroundColor Yellow
} else {
    Write-Host "📌 Installing Orbit $Version" -ForegroundColor Green
}

$Prefix = if ($env:ORBIT_PREFIX) { $env:ORBIT_PREFIX } else { Join-Path $env:ProgramFiles "OrbitFramework" }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and $Prefix.StartsWith($env:ProgramFiles)) {
    Write-Host "⚠️ Installing to $Prefix needs an elevated PowerShell. Re-run as Administrator, or set `$env:ORBIT_PREFIX to a folder you own." -ForegroundColor Yellow
}

# --- Parallelism --------------------------------------------------------------
if ($env:ORBIT_JOBS) {
    $Jobs = [int]$env:ORBIT_JOBS
} else {
    $Cores = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors
    if (-not $Cores) { $Cores = 2 }
    $FreeGB = [math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
    $Jobs = [math]::Max(1, [math]::Min($Cores, [math]::Floor($FreeGB / 2)))
}
Write-Host "🧮 Building with $Jobs parallel job(s) (override with ORBIT_JOBS)."

# --- Workspace ----------------------------------------------------------------
$TmpDir = Join-Path $env:TEMP "orbit_installer_$(Get-Random)"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$OriginalLocation = Get-Location

try {
    Set-Location $TmpDir

    Write-Host "📥 Downloading Orbit $Version..." -ForegroundColor Green
    Invoke-Checked "git clone" { git clone --quiet --depth 1 --branch $Version "https://github.com/$Repo.git" orbit-framework }
    Set-Location orbit-framework

    # --- Dependencies ---------------------------------------------------------
    # Pin vcpkg to the baseline the release was tested with.
    $Baseline = (Get-Content vcpkg.json -Raw | ConvertFrom-Json).'builtin-baseline'
    Write-Host "📦 Setting up vcpkg (baseline $Baseline)..." -ForegroundColor Green
    if ($Baseline) {
        New-Item -ItemType Directory -Path vcpkg | Out-Null
        Invoke-Checked "git init" { git -C vcpkg init --quiet }
        Invoke-Checked "git remote add" { git -C vcpkg remote add origin https://github.com/microsoft/vcpkg.git }
        Invoke-Checked "git fetch" { git -C vcpkg fetch --quiet --depth 1 origin $Baseline }
        Invoke-Checked "git checkout" { git -C vcpkg checkout --quiet FETCH_HEAD }
    } else {
        Invoke-Checked "git clone vcpkg" { git clone --quiet --depth 1 https://github.com/microsoft/vcpkg.git }
    }
    Invoke-Checked "vcpkg bootstrap" { .\vcpkg\bootstrap-vcpkg.bat -disableMetrics }

    # Reuse already-built dependency binaries across installer runs.
    $BinaryCacheDir = if ($env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE } else { Join-Path $env:LOCALAPPDATA "vcpkg-binary-cache" }
    New-Item -ItemType Directory -Force -Path $BinaryCacheDir | Out-Null
    $env:VCPKG_BINARY_SOURCES = "clear;files,$BinaryCacheDir,readwrite"

    # --- Build ----------------------------------------------------------------
    Write-Host "🔨 Building Orbit Framework (this may take a while)..." -ForegroundColor Green
    Invoke-Checked "cmake configure" {
        cmake -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$Prefix" `
            -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake `
            -DORBIT_BUILD_TESTS=OFF -DORBIT_BUILD_EXAMPLES=OFF .
    }
    Invoke-Checked "cmake build" { cmake --build build -j $Jobs --config Release }

    # --- Install --------------------------------------------------------------
    Write-Host "💾 Installing the framework to $Prefix..." -ForegroundColor Green
    Invoke-Checked "cmake install" { cmake --install build --config Release }

    Write-Host "🛠️ Installing the orbit CLI..." -ForegroundColor Green
    $BinDir = Join-Path $Prefix "bin"
    New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
    # Copy the Python script and a batch wrapper so it runs from cmd/PowerShell.
    Copy-Item tools\cli\orbit (Join-Path $BinDir "orbit.py") -Force
    Set-Content -Path (Join-Path $BinDir "orbit.bat") -Value "@echo off`npython `"%~dp0orbit.py`" %*"

    # Add to PATH: machine-wide when elevated, otherwise for this user.
    $Scope = if ($isAdmin) { "Machine" } else { "User" }
    $Path = [Environment]::GetEnvironmentVariable("PATH", $Scope)
    if ($Path -notmatch [regex]::Escape($BinDir)) {
        Write-Host "Adding $BinDir to the $Scope PATH..."
        [Environment]::SetEnvironmentVariable("PATH", "$Path;$BinDir", $Scope)
        Write-Host "⚠️ Restart your terminal for the 'orbit' command to be recognized." -ForegroundColor Yellow
    }
}
finally {
    Set-Location $OriginalLocation
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "✅ Orbit $Version installed to $Prefix." -ForegroundColor Green
Write-Host "Create a project with:"
Write-Host "    orbit new my_project"
Write-Host "    cd my_project"
Write-Host "    orbit build"
Write-Host "    orbit run"
