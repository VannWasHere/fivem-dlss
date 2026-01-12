# Building from Source

This guide walks you through building the FiveM Frame Generation mod from source code.

## Prerequisites

### Required Software

1. **Visual Studio 2022** (Community edition is free)
   - Download from: https://visualstudio.microsoft.com/
   - Required workloads:
     - "Desktop development with C++"
     - "Game development with C++"
   - Required components:
     - Windows 10/11 SDK (10.0.19041.0 or later)
     - MSVC v143 build tools

2. **CMake 3.20+**
   - Download from: https://cmake.org/download/
   - Or install via: `winget install Kitware.CMake`

3. **Git** (optional, for cloning)
   - Download from: https://git-scm.com/

## Quick Build

The easiest way to build is using the provided PowerShell script:

```powershell
# Open PowerShell in the project directory

# Setup dependencies and build
.\setup.ps1 -Build

# Install to FiveM
.\setup.ps1 -Install
```

## Manual Build

### 1. Clone the Repository

```bash
git clone https://github.com/yourusername/fivem-dlss-framegen.git
cd fivem-dlss-framegen
```

### 2. Download Dependencies

Place these libraries in the `deps/` folder:

#### MinHook (Function hooking)
```
deps/minhook/
├── include/
│   └── MinHook.h
└── src/
    └── ...
```
Download from: https://github.com/TsudaKageyu/minhook/releases

#### Dear ImGui (Configuration UI)
```
deps/imgui/
├── imgui.h
├── imgui.cpp
├── backends/
│   ├── imgui_impl_win32.h
│   ├── imgui_impl_win32.cpp
│   ├── imgui_impl_dx11.h
│   └── imgui_impl_dx11.cpp
└── ...
```
Download from: https://github.com/ocornut/imgui/releases

### 3. Configure with CMake

```powershell
# Create build directory
mkdir build
cd build

# Configure (Visual Studio 2022)
cmake .. -G "Visual Studio 17 2022" -A x64

# Or for Visual Studio 2019
cmake .. -G "Visual Studio 16 2019" -A x64
```

### 4. Build

```powershell
# Build Release configuration
cmake --build . --config Release

# The output will be in:
# build/bin/FiveMFrameGen.asi
```

### 5. Install

Copy the compiled ASI to FiveM's plugins folder:

```powershell
copy build\bin\FiveMFrameGen.asi "$env:LOCALAPPDATA\FiveM\FiveM.app\plugins\"
```

## Build Configurations

### Debug Build
```powershell
cmake --build . --config Debug
```
- Includes debug symbols
- Verbose logging
- Suitable for development

### Release Build
```powershell
cmake --build . --config Release
```
- Optimized for performance
- Minimal logging
- Recommended for users

### RelWithDebInfo Build
```powershell
cmake --build . --config RelWithDebInfo
```
- Release optimizations with debug symbols
- Good for profiling

## Troubleshooting

### CMake can't find Visual Studio

Make sure you have the C++ workload installed:
1. Open Visual Studio Installer
2. Click "Modify" on your VS installation
3. Check "Desktop development with C++"
4. Click "Modify" to install

### Linker errors for D3D11

Ensure you have the Windows SDK installed:
1. Open Visual Studio Installer
2. Go to "Individual components"
3. Search for "Windows SDK"
4. Install a recent version

### MinHook not found

Check that MinHook is correctly placed:
```
deps/minhook/include/MinHook.h  <- This file must exist
```

### ImGui errors

Ensure ImGui backends are present:
```
deps/imgui/backends/imgui_impl_dx11.h   <- Required
deps/imgui/backends/imgui_impl_win32.h  <- Required
```

## Development Tips

### Hot Reload (Not Supported)
ASI plugins cannot be hot-reloaded. You must restart FiveM after rebuilding.

### Debug Output
Use `OutputDebugString` for debugging. View output with:
- Visual Studio's Output window (when attached)
- DebugView from Sysinternals

### Logging
Check the log file at:
```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\FiveMFrameGen.log
```

## Project Structure

```
mod-dlss/
├── CMakeLists.txt          # Main build configuration
├── include/
│   └── fivem_framegen.h    # Public API header
├── src/
│   ├── main.cpp            # DLL entry point
│   ├── core/               # DirectX hooking
│   ├── frame_gen/          # Frame generation backends
│   ├── overlay/            # ImGui configuration UI
│   └── utils/              # Logging, config, etc.
├── deps/                   # External dependencies
└── build/                  # Build output (generated)
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

Please follow the existing code style and include appropriate documentation.
