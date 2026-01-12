# FiveM DLSS Frame Generation - Implementation Plan

## Overview

This document outlines the technical implementation strategy for adding DLSS/FSR Frame Generation support to FiveM.

---

## Phase 1: Project Foundation (Week 1-2)

### 1.1 Development Environment Setup

- [ ] Install Visual Studio 2022 with C++ Desktop Development workload
- [ ] Install Windows SDK 10.0.19041.0+
- [ ] Install CMake 3.20+
- [ ] Configure Git with LFS for binary assets

### 1.2 Core Project Structure

```
mod-dlss/
├── CMakeLists.txt                 # Main CMake configuration
├── src/
│   ├── main.cpp                   # DLL entry point
│   ├── core/
│   │   ├── hooks.cpp              # DirectX hooking implementation
│   │   ├── hooks.h
│   │   ├── d3d11_wrapper.cpp      # D3D11 device wrapper
│   │   └── d3d11_wrapper.h
│   ├── frame_gen/
│   │   ├── frame_generator.cpp    # Abstract frame gen interface
│   │   ├── frame_generator.h
│   │   ├── fsr3_backend.cpp       # AMD FSR 3 implementation
│   │   ├── fsr3_backend.h
│   │   ├── optical_flow.cpp       # Motion estimation
│   │   └── optical_flow.h
│   └── overlay/
│       ├── imgui_overlay.cpp      # ImGui integration
│       └── config_ui.cpp          # Configuration interface
├── deps/
│   ├── minhook/                   # Hooking library
│   ├── imgui/                     # Dear ImGui
│   └── fsr3/                      # AMD FidelityFX SDK
└── include/
    └── fivem_framegen.h           # Public API header
```

### 1.3 Dependencies

| Library | Purpose | Source |
|---------|---------|--------|
| MinHook | Function hooking | https://github.com/TsudaKageyu/minhook |
| Dear ImGui | Overlay UI | https://github.com/ocornut/imgui |
| AMD FidelityFX SDK | FSR 3 Frame Generation | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| DirectXTK | D3D11 utilities | https://github.com/microsoft/DirectXTK |

---

## Phase 2: DirectX 11 Hooking (Week 2-3)

### 2.1 Hook Points

The mod needs to intercept these D3D11 functions:

```cpp
// Critical Hook Points
IDXGISwapChain::Present()          // Frame presentation
ID3D11DeviceContext::Draw*()       // Draw calls for depth buffer
ID3D11Device::CreateTexture2D()    // Resource tracking
```

### 2.2 Hooking Strategy

```cpp
// Using MinHook for detouring
#include <MinHook.h>

// Original function pointer
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags
);
Present_t OriginalPresent = nullptr;

// Hook function
HRESULT STDMETHODCALLTYPE HookedPresent(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags
) {
    // Pre-present: Capture current frame
    g_FrameGenerator->CaptureFrame(pSwapChain);
    
    // Original present
    HRESULT hr = OriginalPresent(pSwapChain, SyncInterval, Flags);
    
    // Post-present: Generate and present interpolated frame
    if (g_FrameGenEnabled) {
        g_FrameGenerator->GenerateAndPresentFrame(pSwapChain);
    }
    
    return hr;
}
```

### 2.3 Device Discovery

```cpp
bool InitializeHooks() {
    // Method 1: Hook D3D11CreateDevice to get device at creation
    // Method 2: Find existing device through window enumeration
    
    HWND gameWindow = FindWindow(NULL, L"FiveM");
    if (!gameWindow) return false;
    
    // Create dummy swap chain to get vtable
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = gameWindow;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    
    ID3D11Device* pDevice;
    IDXGISwapChain* pSwapChain;
    ID3D11DeviceContext* pContext;
    
    D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &pSwapChain, &pDevice, nullptr, &pContext
    );
    
    // Get vtable and hook
    void** pSwapChainVtable = *reinterpret_cast<void***>(pSwapChain);
    OriginalPresent = (Present_t)pSwapChainVtable[8]; // Present is index 8
    
    MH_CreateHook(OriginalPresent, HookedPresent, (void**)&OriginalPresent);
    MH_EnableHook(MH_ALL_HOOKS);
    
    // Cleanup dummy resources
    pSwapChain->Release();
    pDevice->Release();
    pContext->Release();
    
    return true;
}
```

---

## Phase 3: Frame Generation Core (Week 3-5)

### 3.1 Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Frame Generation Pipeline                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────┐    ┌──────────────┐    ┌─────────────────────┐   │
│  │ Frame N │───▶│ Optical Flow │───▶│ Motion Vector Calc   │   │
│  └─────────┘    │   Analysis   │    │                      │   │
│       │         └──────────────┘    └──────────┬───────────┘   │
│       │                                         │               │
│       │         ┌──────────────┐    ┌──────────▼───────────┐   │
│       └────────▶│ Frame N+1   │───▶│  Frame Interpolation │   │
│                 │  (Next Real) │    │      (AI/GPU)        │   │
│                 └──────────────┘    └──────────┬───────────┘   │
│                                                 │               │
│                                     ┌──────────▼───────────┐   │
│                                     │  Generated Frame     │   │
│                                     │   (Interpolated)     │   │
│                                     └──────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Optical Flow Implementation

Since GTA V doesn't provide motion vectors, we need to compute them:

```cpp
class OpticalFlowCalculator {
public:
    struct MotionVector {
        float x, y;
    };
    
    // Calculate motion between two frames
    void CalculateMotion(
        ID3D11Texture2D* framePrev,
        ID3D11Texture2D* frameCurrent,
        ID3D11Texture2D* motionVectors  // Output
    ) {
        // Use compute shader for GPU-accelerated optical flow
        // Block matching algorithm or Lucas-Kanade method
        
        m_Context->CSSetShader(m_OpticalFlowCS.Get(), nullptr, 0);
        m_Context->CSSetShaderResources(0, 1, &framePrevSRV);
        m_Context->CSSetShaderResources(1, 1, &frameCurrentSRV);
        m_Context->CSSetUnorderedAccessViews(0, 1, &motionVectorsUAV, nullptr);
        m_Context->Dispatch(
            (width + 15) / 16,
            (height + 15) / 16,
            1
        );
    }

private:
    ComPtr<ID3D11ComputeShader> m_OpticalFlowCS;
    ComPtr<ID3D11DeviceContext> m_Context;
};
```

### 3.3 FSR 3 Frame Generation Integration

```cpp
#include <ffx_frameinterpolation.h>

class FSR3FrameGenerator : public IFrameGenerator {
public:
    bool Initialize(ID3D11Device* device) override {
        // Create FSR 3 context
        FfxFrameInterpolationContextDescription desc = {};
        desc.flags = FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INTERPOLATION;
        desc.maxRenderSize = { m_Width, m_Height };
        desc.displaySize = { m_Width, m_Height };
        
        FfxErrorCode result = ffxFrameInterpolationContextCreate(
            &m_Context,
            &desc
        );
        
        return result == FFX_OK;
    }
    
    void GenerateFrame(
        ID3D11Texture2D* currentFrame,
        ID3D11Texture2D* previousFrame,
        ID3D11Texture2D* motionVectors,
        ID3D11Texture2D* outputFrame
    ) override {
        FfxFrameInterpolationDispatchDescription dispatchDesc = {};
        dispatchDesc.commandList = m_CommandList;
        dispatchDesc.currentBackBuffer = WrapResource(currentFrame);
        dispatchDesc.previousBackBuffer = WrapResource(previousFrame);
        dispatchDesc.motionVectors = WrapResource(motionVectors);
        dispatchDesc.output = WrapResource(outputFrame);
        dispatchDesc.reset = m_FirstFrame;
        
        ffxFrameInterpolationDispatch(&m_Context, &dispatchDesc);
        m_FirstFrame = false;
    }

private:
    FfxFrameInterpolationContext m_Context;
    ComPtr<ID3D11CommandList> m_CommandList;
    bool m_FirstFrame = true;
};
```

---

## Phase 4: ASI Plugin Integration (Week 5-6)

### 4.1 DLL Entry Point

```cpp
// main.cpp
#include <Windows.h>
#include "core/hooks.h"
#include "frame_gen/frame_generator.h"
#include "overlay/imgui_overlay.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

HMODULE g_hModule = nullptr;
std::unique_ptr<FrameGenerator> g_FrameGenerator;
std::unique_ptr<ImGuiOverlay> g_Overlay;

void InitializeMod() {
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        MessageBox(nullptr, L"Failed to initialize MinHook!", L"Error", MB_OK);
        return;
    }
    
    // Wait for game initialization
    while (!FindWindow(nullptr, L"FiveM")) {
        Sleep(100);
    }
    Sleep(5000); // Wait for D3D11 to initialize
    
    // Initialize hooks
    if (!InitializeD3D11Hooks()) {
        MessageBox(nullptr, L"Failed to hook D3D11!", L"Error", MB_OK);
        return;
    }
    
    // Initialize frame generator
    g_FrameGenerator = std::make_unique<FSR3FrameGenerator>();
    if (!g_FrameGenerator->Initialize(g_Device)) {
        MessageBox(nullptr, L"Failed to initialize FSR3!", L"Error", MB_OK);
        return;
    }
    
    // Initialize overlay
    g_Overlay = std::make_unique<ImGuiOverlay>();
    g_Overlay->Initialize(g_Device, g_Context);
}

void CleanupMod() {
    g_Overlay.reset();
    g_FrameGenerator.reset();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

// DLL Main Entry
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)InitializeMod, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        CleanupMod();
    }
    return TRUE;
}
```

### 4.2 FiveM Plugin Directory

Place the compiled `.asi` file in:
```
%LocalAppData%\FiveM\FiveM.app\plugins\
```

---

## Phase 5: Configuration UI (Week 6-7)

### 5.1 ImGui Overlay

```cpp
class ConfigUI {
public:
    void Render() {
        if (!m_Visible) return;
        
        ImGui::Begin("Frame Generation Settings", &m_Visible);
        
        // Enable/Disable
        ImGui::Checkbox("Enable Frame Generation", &g_FrameGenEnabled);
        
        // Backend selection
        const char* backends[] = { "FSR 3", "DLSS 3 (RTX 40 only)" };
        ImGui::Combo("Backend", &m_SelectedBackend, backends, 2);
        
        // Quality preset
        const char* quality[] = { "Performance", "Balanced", "Quality" };
        ImGui::Combo("Quality", &m_QualityLevel, quality, 3);
        
        // Frame pacing
        ImGui::SliderFloat("Target Framerate", &m_TargetFPS, 30.0f, 144.0f);
        
        // Stats display
        ImGui::Separator();
        ImGui::Text("Performance:");
        ImGui::Text("Base FPS: %.1f", m_BaseFPS);
        ImGui::Text("Output FPS: %.1f", m_OutputFPS);
        ImGui::Text("Frame Time: %.2f ms", m_FrameTimeMs);
        
        ImGui::End();
    }
    
    void Toggle() { m_Visible = !m_Visible; }

private:
    bool m_Visible = false;
    int m_SelectedBackend = 0;
    int m_QualityLevel = 1;
    float m_TargetFPS = 60.0f;
    float m_BaseFPS = 0.0f;
    float m_OutputFPS = 0.0f;
    float m_FrameTimeMs = 0.0f;
};
```

---

## Phase 6: Testing & Optimization (Week 7-8)

### 6.1 Test Cases

| Test | Description | Expected Result |
|------|-------------|-----------------|
| Basic Load | DLL loads without crash | Game starts normally |
| Frame Gen Toggle | Enable/disable frame gen | FPS doubles when enabled |
| Quality Modes | Switch between presets | Visual quality changes |
| Stability | 1 hour gameplay session | No crashes or memory leaks |
| Compatibility | Test on various GPUs | Works on all modern GPUs |

### 6.2 Performance Metrics

```cpp
class PerformanceMonitor {
public:
    void BeginFrame() {
        QueryPerformanceCounter(&m_FrameStart);
    }
    
    void EndFrame() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        
        double frameTime = double(end.QuadPart - m_FrameStart.QuadPart) 
                         / m_Frequency.QuadPart * 1000.0;
        
        m_FrameTimes.push_back(frameTime);
        if (m_FrameTimes.size() > 100) {
            m_FrameTimes.erase(m_FrameTimes.begin());
        }
        
        // Calculate average
        m_AverageFrameTime = 0;
        for (auto& t : m_FrameTimes) {
            m_AverageFrameTime += t;
        }
        m_AverageFrameTime /= m_FrameTimes.size();
        m_FPS = 1000.0 / m_AverageFrameTime;
    }
    
    double GetFPS() const { return m_FPS; }
    double GetFrameTime() const { return m_AverageFrameTime; }

private:
    LARGE_INTEGER m_FrameStart;
    LARGE_INTEGER m_Frequency;
    std::vector<double> m_FrameTimes;
    double m_AverageFrameTime = 0;
    double m_FPS = 0;
};
```

---

## Technical Considerations

### Challenges & Solutions

| Challenge | Solution |
|-----------|----------|
| No native motion vectors | GPU-computed optical flow |
| DirectX 11 limitation | FSR 3 works on DX11 via compute shaders |
| Frame pacing | VSync management + frame limiter |
| Input latency | NVIDIA Reflex-like optimizations |
| Memory management | Ring buffer for frame history |

### Known Limitations

1. **Ghosting artifacts**: Fast-moving objects may show ghosting
2. **UI artifacts**: HUD elements might interpolate incorrectly
3. **Input latency**: +1 frame of latency (mitigated with Reflex)
4. **Compatibility**: May conflict with other graphics mods

---

## Resources

- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
- [NVIDIA Streamline SDK](https://developer.nvidia.com/streamline)
- [MinHook Library](https://github.com/TsudaKageyu/minhook)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [DirectX 11 Documentation](https://docs.microsoft.com/en-us/windows/win32/direct3d11)
