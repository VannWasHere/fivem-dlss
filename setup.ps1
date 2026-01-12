# Setup script for FiveM Frame Generation development environment

param(
    [switch]$Clean,
    [switch]$Build,
    [switch]$Install
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$DepsDir = Join-Path $ProjectRoot "deps"
$BuildDir = Join-Path $ProjectRoot "build"

Write-Host "FiveM Frame Generation - Development Setup" -ForegroundColor Cyan
Write-Host "===========================================" -ForegroundColor Cyan
Write-Host ""

# Clean build directory
if ($Clean) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
}

# Download and setup dependencies
function Setup-Dependencies {
    Write-Host "Setting up dependencies..." -ForegroundColor Yellow
    
    if (-not (Test-Path $DepsDir)) {
        New-Item -ItemType Directory -Path $DepsDir | Out-Null
    }
    
    # MinHook
    $MinHookDir = Join-Path $DepsDir "minhook"
    if (-not (Test-Path $MinHookDir)) {
        Write-Host "  Downloading MinHook..." -ForegroundColor Gray
        $MinHookZip = Join-Path $DepsDir "minhook.zip"
        Invoke-WebRequest -Uri "https://github.com/TsudaKageyu/minhook/releases/download/v1.3.3/MinHook_133_src.zip" -OutFile $MinHookZip
        Expand-Archive -Path $MinHookZip -DestinationPath $DepsDir
        Rename-Item (Join-Path $DepsDir "MinHook_133_src") $MinHookDir
        Remove-Item $MinHookZip
        Write-Host "  MinHook downloaded!" -ForegroundColor Green
    } else {
        Write-Host "  MinHook already present" -ForegroundColor Gray
    }
    
    # ImGui
    $ImGuiDir = Join-Path $DepsDir "imgui"
    if (-not (Test-Path $ImGuiDir)) {
        Write-Host "  Downloading ImGui..." -ForegroundColor Gray
        $ImGuiZip = Join-Path $DepsDir "imgui.zip"
        Invoke-WebRequest -Uri "https://github.com/ocornut/imgui/archive/refs/tags/v1.90.1.zip" -OutFile $ImGuiZip
        Expand-Archive -Path $ImGuiZip -DestinationPath $DepsDir
        Rename-Item (Join-Path $DepsDir "imgui-1.90.1") $ImGuiDir
        Remove-Item $ImGuiZip
        Write-Host "  ImGui downloaded!" -ForegroundColor Green
    } else {
        Write-Host "  ImGui already present" -ForegroundColor Gray
    }
    
    Write-Host "Dependencies ready!" -ForegroundColor Green
}

# Create CMakeLists for dependencies
function Create-DependencyCMake {
    Write-Host "Creating dependency build files..." -ForegroundColor Yellow
    
    # MinHook CMakeLists.txt
    $MinHookCMake = @"
cmake_minimum_required(VERSION 3.20)
project(minhook)

add_library(minhook STATIC
    src/buffer.c
    src/hook.c
    src/trampoline.c
    src/hde/hde32.c
    src/hde/hde64.c
)

target_include_directories(minhook PUBLIC include)
"@
    $MinHookCMake | Out-File -FilePath (Join-Path $DepsDir "minhook\CMakeLists.txt") -Encoding UTF8
    
    # ImGui CMakeLists.txt
    $ImGuiCMake = @"
cmake_minimum_required(VERSION 3.20)
project(imgui)

add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_win32.cpp
    backends/imgui_impl_dx11.cpp
)

target_include_directories(imgui PUBLIC 
    `${CMAKE_CURRENT_SOURCE_DIR}
    `${CMAKE_CURRENT_SOURCE_DIR}/backends
)

target_link_libraries(imgui PRIVATE d3d11 dxgi)
"@
    $ImGuiCMake | Out-File -FilePath (Join-Path $DepsDir "imgui\CMakeLists.txt") -Encoding UTF8
    
    Write-Host "Dependency build files created!" -ForegroundColor Green
}

# Build project
function Build-Project {
    Write-Host "Building project..." -ForegroundColor Yellow
    
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }
    
    Push-Location $BuildDir
    
    try {
        # Configure
        Write-Host "  Configuring CMake..." -ForegroundColor Gray
        cmake .. -G "Visual Studio 17 2022" -A x64
        
        # Build
        Write-Host "  Building..." -ForegroundColor Gray
        cmake --build . --config Release
        
        Write-Host "Build complete!" -ForegroundColor Green
        Write-Host "Output: $(Join-Path $BuildDir 'bin\FiveMFrameGen.asi')" -ForegroundColor Cyan
    }
    finally {
        Pop-Location
    }
}

# Install to FiveM
function Install-ToFiveM {
    Write-Host "Installing to FiveM..." -ForegroundColor Yellow
    
    $FiveMPlugins = Join-Path $env:LOCALAPPDATA "FiveM\FiveM.app\plugins"
    
    if (-not (Test-Path $FiveMPlugins)) {
        Write-Host "  Creating plugins directory..." -ForegroundColor Gray
        New-Item -ItemType Directory -Path $FiveMPlugins | Out-Null
    }
    
    $AsiFile = Join-Path $BuildDir "bin\FiveMFrameGen.asi"
    
    if (Test-Path $AsiFile) {
        Copy-Item $AsiFile -Destination $FiveMPlugins -Force
        Write-Host "Installed to: $FiveMPlugins" -ForegroundColor Green
    } else {
        Write-Host "ASI file not found. Build the project first!" -ForegroundColor Red
    }
}

# Main execution
Write-Host "Checking prerequisites..." -ForegroundColor Yellow

# Check for Visual Studio
$VSWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $VSWhere) {
    $VSPath = & $VSWhere -latest -property installationPath
    if ($VSPath) {
        Write-Host "  Visual Studio found at: $VSPath" -ForegroundColor Green
    } else {
        Write-Host "  Visual Studio not found!" -ForegroundColor Red
        Write-Host "  Please install Visual Studio 2022 with C++ workload" -ForegroundColor Yellow
        exit 1
    }
} else {
    Write-Host "  Visual Studio Installer not found" -ForegroundColor Yellow
}

# Check for CMake
$CMakeVersion = cmake --version 2>$null
if ($CMakeVersion) {
    Write-Host "  CMake found: $($CMakeVersion[0])" -ForegroundColor Green
} else {
    Write-Host "  CMake not found!" -ForegroundColor Red
    Write-Host "  Please install CMake 3.20 or later" -ForegroundColor Yellow
    exit 1
}

Write-Host ""

# Run requested operations
Setup-Dependencies
Create-DependencyCMake

if ($Build) {
    Build-Project
}

if ($Install) {
    Install-ToFiveM
}

Write-Host ""
Write-Host "Setup complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Run: .\setup.ps1 -Build" -ForegroundColor White
Write-Host "  2. Run: .\setup.ps1 -Install" -ForegroundColor White
Write-Host "  3. Launch FiveM and press F10 to open settings" -ForegroundColor White
