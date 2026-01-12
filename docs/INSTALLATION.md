# Installation Guide

This guide explains how to install and configure the FiveM Frame Generation mod.

## Quick Installation

### 1. Download

Download the latest release:
- **FiveMFrameGen.asi** - The main mod file
- **FiveMFrameGen.ini** - Optional configuration file

### 2. Locate FiveM Plugins Folder

The plugins folder is located at:
```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\
```

To open it quickly:
1. Press `Win + R`
2. Paste: `%LOCALAPPDATA%\FiveM\FiveM.app\plugins`
3. Press Enter

If the `plugins` folder doesn't exist, create it.

### 3. Install Files

Copy `FiveMFrameGen.asi` to the plugins folder:
```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\FiveMFrameGen.asi
```

### 4. Launch FiveM

1. Start FiveM as normal
2. Join any server or enter single-player mode
3. Press **F10** to open the configuration overlay

## Requirements

### Hardware

| Feature | Minimum | Recommended |
|---------|---------|-------------|
| GPU (FSR 3) | GTX 1060 / RX 580 | RTX 3060 / RX 6600 |
| GPU (DLSS 3) | RTX 4060 | RTX 4070+ |
| VRAM | 4 GB | 8 GB |
| RAM | 8 GB | 16 GB |

### Software

- **Windows 10** version 20H1 (build 19041) or later
- **Windows 11** any version
- **FiveM** latest version
- **GPU Drivers** (latest recommended)
  - NVIDIA: 535.0+ for DLSS 3
  - AMD: 23.3+ for FSR 3
  - Intel: latest for XeSS

### Windows Settings

**Enable Hardware-Accelerated GPU Scheduling:**
1. Open Settings > System > Display
2. Click "Graphics settings"
3. Turn on "Hardware-accelerated GPU scheduling"
4. Restart your PC

## Configuration

### In-Game Menu (F10)

Press **F10** while in-game to open the configuration overlay:

| Setting | Description |
|---------|-------------|
| Enable Frame Generation | Turn frame gen on/off |
| Backend | FSR 3 (all GPUs) or DLSS 3 (RTX 40 only) |
| Quality | Performance / Balanced / Quality |
| Sharpness | Adjust output sharpness (0-100%) |
| HUD-less Mode | Reduce HUD artifacts |

### Hotkeys

| Key | Action |
|-----|--------|
| F9 | Toggle Frame Generation On/Off |
| F10 | Open/Close Settings Menu |

### Configuration File

Settings are saved to:
```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\FiveMFrameGen.ini
```

Example configuration:
```ini
[General]
Enabled=true
Backend=1
Quality=1
TargetFramerate=60.000000
ShowOverlay=true
HudLessMode=false
Sharpness=0.500000
```

**Backend values:**
- 0 = None (disabled)
- 1 = FSR 3 (recommended for most users)
- 2 = DLSS 3 (RTX 40 series only)
- 3 = Optical Flow

**Quality values:**
- 0 = Performance (fastest)
- 1 = Balanced (recommended)
- 2 = Quality (best visual)

## Graphics Settings

### Recommended In-Game Settings

For best frame generation results:

| Setting | Recommendation |
|---------|----------------|
| FXAA | **Off** |
| MSAA | **Off** |
| Motion Blur | **Off** |
| VSync | **Off** (use driver VSync if needed) |
| Frame Limiter | **Off** |

### Why Disable AA?

Frame generation works best with clean input frames. Built-in anti-aliasing can cause:
- Ghosting artifacts
- Blurry output
- Reduced motion clarity

Use upscaling AA instead (FSR 2/DLSS 2) if available.

## Performance Tips

### Maximize Frame Generation Benefits

1. **Target 40+ base FPS**: Frame gen works best with stable input
2. **Reduce CPU bottlenecks**: Frame gen helps GPU-limited scenarios
3. **Use Balanced quality**: Best performance/quality ratio
4. **Disable overlays**: Other overlays may cause conflicts

### Troubleshooting Low FPS

| Issue | Solution |
|-------|----------|
| Low base FPS | Reduce graphics settings |
| Stuttering | Enable VSync, reduce VRAM usage |
| High latency | Use Performance preset |
| GPU not reaching 100% | CPU bottleneck, reduce draw distance |

## Compatibility

### Supported

- ✅ FiveM single-player mode
- ✅ FiveM servers (without anti-cheat)
- ✅ Most graphics mods (ENB, ReShade)
- ✅ All modern NVIDIA GPUs (GTX 900+)
- ✅ All modern AMD GPUs (RX 400+)
- ✅ Intel Arc GPUs

### Not Recommended

- ⚠️ Servers with anti-cheat (may cause bans)
- ⚠️ ReShade depth buffer effects (may conflict)
- ⚠️ Multiple frame generation mods

### Known Issues

| Issue | Workaround |
|-------|-----------|
| HUD ghosting | Enable HUD-less Mode |
| Particle artifacts | Reduce Quality preset |
| Screen tearing | Enable VSync in driver |

## Uninstallation

1. Close FiveM completely
2. Delete `FiveMFrameGen.asi` from plugins folder
3. Optionally delete `FiveMFrameGen.ini` and `FiveMFrameGen.log`

## Support

### Log Files

If you encounter issues, check the log file:
```
%LOCALAPPDATA%\FiveM\FiveM.app\plugins\FiveMFrameGen.log
```

### Reporting Issues

When reporting issues, please include:
1. Your GPU model and driver version
2. Windows version
3. FiveM version
4. Contents of the log file
5. Steps to reproduce the issue

## FAQ

**Q: Will I get banned for using this?**
A: This mod modifies graphics only. However, some servers with aggressive anti-cheat may flag any modifications. Use at your own risk on servers with anti-cheat.

**Q: Why is my FPS only slightly higher?**
A: Frame generation doubles your rendered FPS. If you're CPU-limited, the benefit will be smaller.

**Q: Can I use this with ENB/ReShade?**
A: Yes, but load order matters. ENB/ReShade should load after this mod.

**Q: Does this work with GTA Online?**
A: No, this is for FiveM only and does not modify GTA Online in any way.
