# FiveM DLSS Frame Generation Mod

A mod that enables Frame Generation technology for FiveM (GTA V multiplayer), providing significant FPS improvements using NVIDIA DLSS 3 or AMD FSR 3.

## 🎮 Overview

FiveM uses an older GTA V build that doesn't natively support modern Frame Generation technologies. This mod bridges that gap by injecting Frame Generation capabilities into the FiveM rendering pipeline.

## ⚡ Features

- **Frame Generation**: Generate intermediate frames using AI for up to 2x performance boost
- **Multiple Backend Support**: 
  - AMD FSR 3 Frame Generation (all GPUs)
  - NVIDIA DLSS 3 Frame Generation (RTX 40 series only)
- **Upscaling Options**: FSR 2.2, DLSS 2, XeSS integration
- **In-Game UI**: Toggle and configure via overlay menu
- **FiveM Compatible**: Works with FiveM's ASI loader system

## 📋 Requirements

### Hardware
- **For FSR 3 Frame Gen**: Any modern GPU (NVIDIA GTX 1000+, AMD RX 5000+, Intel Arc)
- **For DLSS 3 Frame Gen**: NVIDIA RTX 40 series GPU only

### Software
- Windows 10 20H1 (build 19041) or newer
- FiveM client (latest version)
- Hardware-Accelerated GPU Scheduling enabled
- Latest GPU drivers

## 🔧 Installation

1. Download the latest release from the Releases page
2. Copy files to your FiveM plugins folder:
   ```
   %LocalAppData%\FiveM\FiveM.app\plugins\
   ```
3. Launch FiveM
4. Press **F10** to open the configuration overlay

## 🏗️ Building from Source

### Prerequisites
- Visual Studio 2022 with C++ workload
- Windows SDK 10.0.19041.0 or later
- CMake 3.20+
- Git

### Build Steps
```powershell
git clone https://github.com/yourusername/fivem-dlss-framegen.git
cd fivem-dlss-framegen
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## 📁 Project Structure

```
mod-dlss/
├── src/
│   ├── core/           # Core injection and hooking logic
│   ├── frame_gen/      # Frame generation implementation
│   ├── upscalers/      # Upscaling backends (FSR, DLSS, XeSS)
│   ├── overlay/        # ImGui-based configuration overlay
│   └── utils/          # Utilities and helpers
├── deps/               # External dependencies
├── include/            # Public headers
├── docs/               # Documentation
└── tools/              # Build and packaging scripts
```

## ⚠️ Disclaimer

This mod is for **single-player FiveM experiences only**. Using graphics mods on servers with anti-cheat may result in bans. Always check server rules before using modifications.

## 📄 License

MIT License - See LICENSE file for details

## 🙏 Credits

- NVIDIA for DLSS technology
- AMD for FSR technology
- OptiScaler project for inspiration
- FiveM community
