/**
 * FiveM Frame Generation Mod
 * Main entry point for the ASI plugin
 * 
 * This mod enables DLSS 3 / FSR 3 Frame Generation for FiveM
 */

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <thread>
#include <memory>
#include <string>

#include "core/hooks.h"
#include "frame_gen/frame_generator.h"
#include "overlay/imgui_overlay.h"
#include "utils/logger.h"
#include "utils/config.h"
#include "fivem_framegen.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
    // Global module handle
    HMODULE g_hModule = nullptr;
    
    // Core components
    std::unique_ptr<FiveMFrameGen::Core::Hooks> g_Hooks;
    std::unique_ptr<FiveMFrameGen::FrameGen::IFrameGenerator> g_FrameGenerator;
    std::unique_ptr<FiveMFrameGen::Overlay::ImGuiOverlay> g_Overlay;
    std::unique_ptr<FiveMFrameGen::Utils::ConfigManager> g_Config;
    
    // State
    bool g_Initialized = false;
    
    // Error handling
    std::string g_LastError;
    
    // Output FPS with frame gen
    constexpr UINT OVERLAY_TOGGLE_KEY = VK_F10;
    constexpr UINT FRAMEGEN_TOGGLE_KEY = VK_F9;
}

// Global Definitions (External Linkage)
namespace FiveMFrameGen {
    Config g_FrameGenConfig;
    Stats g_Stats = {};
}

// Access local aliases
using FiveMFrameGen::g_FrameGenConfig;
using FiveMFrameGen::g_Stats;

// Forward declaration
void LogRaw(const char* fmt, ...);

/**
 * Find the FiveM game window
 */
HWND FindFiveMWindow() {
    // FiveM window titles can vary
    const wchar_t* windowTitles[] = {
        L"FiveM",
        L"FiveM®",
        L"Grand Theft Auto V"
    };
    
    for (const auto& title : windowTitles) {
        HWND hwnd = FindWindowW(nullptr, title);
        if (hwnd != nullptr) {
            return hwnd;
        }
    }
    
    // Fallback: enumerate windows
    struct EnumData {
        HWND result;
    } data = { nullptr };
    
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* data = reinterpret_cast<EnumData*>(lParam);
        
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        
        if (pid == GetCurrentProcessId()) {
            wchar_t title[256];
            GetWindowTextW(hwnd, title, 256);
            
            // Check if this looks like the game window
            if (wcsstr(title, L"FiveM") || wcsstr(title, L"GTA")) {
                data->result = hwnd;
                return FALSE;
            }
        }
        
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    
    return data.result;
}

/**
 * Initialize the mod
 */
void InitializeModSafe();

void InitializeMod() {
    LogRaw("InitializeMod: Starting");
    // Basic structured exception handling for crash diagnosis
    __try {
        InitializeModSafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogRaw("FATAL EXCEPTION in InitializeMod! Code: %d", GetExceptionCode());
    }
    LogRaw("InitializeMod: Finished");
}

// Deprecated GDI Window code removed. ImGui Overlay is now used.

void InputLoop() {
    LogRaw("InputLoop: Running (Background Handler)...");
    
    // We don't need to create a window anymore.
    // The ImGui Overlay hooks the game window directly.
    
    while (g_Initialized) {
        // F9 - Toggle Frame Gen / Upscaling
        if (GetAsyncKeyState(VK_F9) & 1) {
            g_FrameGenConfig.enabled = !g_FrameGenConfig.enabled;
            LogRaw("Hotkey: F9 pressed (FrameGen %s)", g_FrameGenConfig.enabled ? "Enabled" : "Disabled");
        }
        
        // F10 - Toggle Overlay (Fallback)
        if (GetAsyncKeyState(VK_F10) & 1) {
            if (g_Overlay) {
                g_Overlay->Toggle();
                LogRaw("Hotkey: F10 pressed (Overlay Toggled)");
            } else {
                LogRaw("Hotkey: F10 pressed but g_Overlay is NULL!");
            }
        }
        
        // Monitor config changes to backend
        static FiveMFrameGen::QualityPreset lastQuality = g_FrameGenConfig.quality;
        if (g_FrameGenConfig.quality != lastQuality) {
            SetD3D12Quality((int)g_FrameGenConfig.quality);
            lastQuality = g_FrameGenConfig.quality;
        }

        Sleep(50);
    }
    
    LogRaw("InputLoop exited");
}

// Forward declaration
void SetD3D12Overlay(void* overlay);

// ...

void InitializeModSafe() {
    LogRaw("InitializeModSafe: Step 1 - Entry");
    
    try {
        LogRaw("InitializeModSafe: Step 2 - Waiting for window");
        
        HWND gameWindow = nullptr;
        for (int i = 0; i < 100; i++) {
            gameWindow = FindFiveMWindow();
            if (gameWindow) break;
            Sleep(100);
            if (i % 20 == 0) LogRaw("Searching for window... attempt %d", i);
        }
        
        if (!gameWindow) {
            LogRaw("ERROR: Could not find game window");
            return;
        }
        
        // Create Overlay Instance early
        g_Overlay = std::make_unique<FiveMFrameGen::Overlay::ImGuiOverlay>();
        
        // Check if D3D12 is being used
        bool useD3D12 = GetModuleHandleA("d3d12.dll") != nullptr;
        LogRaw("InitializeModSafe: Step 4 - D3D12 detected: %s", useD3D12 ? "YES" : "NO");
        
        Sleep(3000); // Wait for DX
        
        if (useD3D12) {
            LogRaw("InitializeModSafe: Using D3D12 path");
            
            if (InitD3D12Hooks(gameWindow)) {
                LogRaw("InitializeModSafe: D3D12 hooks installed!");
                
                // Pass Overlay to D3D12 Backend
                SetD3D12Overlay(g_Overlay.get());
                
                g_Initialized = true;
                // Enable by default
                g_FrameGenConfig.showOverlay = true;
                
                InputLoop();
                return;
            }
            LogRaw("ERROR: D3D12 Hooks failed, trying D3D11 fallback...");
            useD3D12 = false;
        }
        
        // D3D11 Path
        LogRaw("InitializeModSafe: Using D3D11 path");
        g_Hooks = std::make_unique<FiveMFrameGen::Core::Hooks>();
        
        if (!g_Hooks->Initialize(gameWindow)) {
            LogRaw("ERROR: Hooks Init Failed");
            return;
        }
        
        LogRaw("InitializeModSafe: Step 7 - Waiting for Device (from Present hook)");
        
        // The device is captured on the first Present() call
        // We need to wait for it
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain* swapChain = nullptr;
        
        for (int i = 0; i < 100; i++) {  // Wait up to 10 seconds
            device = g_Hooks->GetDevice();
            context = g_Hooks->GetContext();
            swapChain = g_Hooks->GetSwapChain();
            
            if (device && context && swapChain) {
                break;
            }
            
            Sleep(100);
            if (i % 20 == 0) LogRaw("Waiting for D3D11 device... attempt %d", i);
        }
        
        if (!device || !context || !swapChain) {
            LogRaw("ERROR: No D3D11 Device after 10s wait");
            return;
        }
        LogRaw("InitializeModSafe: Step 8 - Got Device: 0x%p", device);
        
        LogRaw("InitializeModSafe: Step 9 - Creating Overlay");
        g_Overlay = std::make_unique<FiveMFrameGen::Overlay::ImGuiOverlay>();
        
        LogRaw("InitializeModSafe: Step 10 - Initializing Overlay");
        if (!g_Overlay->Initialize(device, context, gameWindow)) {
            LogRaw("WARNING: Overlay Init Failed");
        }
        
        // Enable overlay by default
        g_FrameGenConfig.showOverlay = true;
        
        // Set present callback - Pass render target and render
        LogRaw("InitializeModSafe: Step 11 - Setting Callback");
        g_Hooks->SetPresentCallback([](IDXGISwapChain* sc) {
            if (g_Overlay && g_Hooks) {
                // Pass the render target from hooks to overlay
                g_Overlay->SetRenderTarget(g_Hooks->GetRenderTargetView());
                g_Overlay->Render(g_FrameGenConfig, g_Stats);
            }
        });
        
        g_Initialized = true;
        LogRaw("InitializeModSafe: Step 12 - SUCCESS! Starting InputLoop");
        
        // Start input loop
        InputLoop();
        
        LogRaw("InitializeModSafe: Done");
    }
    catch (const std::exception& e) {
        LogRaw("C++ Exception: %s", e.what());
    }
    catch (...) {
        LogRaw("Unknown C++ Exception");
    }
}

/**
 * Cleanup the mod
 */
void CleanupMod() {
    FiveMFrameGen::Utils::Logger::Info("Shutting down FiveM Frame Generation Mod...");
    
    // Save configuration
    if (g_Config) {
        g_Config->Save(g_FrameGenConfig);
    }
    
    // Cleanup in reverse order
    g_Overlay.reset();
    g_FrameGenerator.reset();
    g_Hooks.reset();
    g_Config.reset();
    
    g_Initialized = false;
    
    FiveMFrameGen::Utils::Logger::Info("Shutdown complete");
    FiveMFrameGen::Utils::Logger::Shutdown();
}

/**
 * Keyboard hook for hotkeys
 */
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        
        if (kb->vkCode == OVERLAY_TOGGLE_KEY) {
            if (g_Overlay) {
                g_Overlay->Toggle();
            }
        }
        else if (kb->vkCode == FRAMEGEN_TOGGLE_KEY) {
            g_FrameGenConfig.enabled = !g_FrameGenConfig.enabled;
            FiveMFrameGen::Utils::Logger::Info("Frame generation %s",
                g_FrameGenConfig.enabled ? "enabled" : "disabled");
        }
    }
    
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// RAW DEBUG LOGGER
void LogRaw(const char* fmt, ...) {
    FILE* f = fopen("C:\\Users\\Justine Donovan\\Desktop\\FiveM_DEBUG_LOAD.txt", "a");
    if (!f) return;
    
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    fprintf(f, "\n");
    va_end(args);
    fclose(f);
}

// Wrapper for checking if we should continue
bool ShouldRun() {
    return true; // We are in a detached thread, run until done or game closes
}

DWORD WINAPI InitThreadProc(LPVOID lpParam) {
    LogRaw("InitThreadProc started");
    InitializeMod();
    LogRaw("InitThreadProc finished");
    return 0;
}

/**
 * DLL entry point
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            
            // IMMEDIATE DEBUG
            {
                FILE* f = fopen("C:\\Users\\Justine Donovan\\Desktop\\FiveM_DEBUG_LOAD.txt", "w");
                if (f) {
                    fprintf(f, "DLL_PROCESS_ATTACH called at %llu\n", GetTickCount64());
                    fclose(f);
                }
            }
            
            // Use CreateThread instead of std::thread to be safer in DllMain
            CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
            break;
            
        case DLL_PROCESS_DETACH:
            CleanupMod();
            break;
    }
    
    return TRUE;
}

// ============================================================================
// Public API Implementation
// ============================================================================

namespace FiveMFrameGen {

FRAMEGEN_API bool Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    IDXGISwapChain* swapChain
) {
    // This is called automatically by DllMain
    // Manual initialization not currently supported
    return g_Initialized;
}

FRAMEGEN_API void Shutdown() {
    CleanupMod();
}

FRAMEGEN_API bool IsInitialized() {
    return g_Initialized;
}

FRAMEGEN_API void SetEnabled(bool enabled) {
    g_FrameGenConfig.enabled = enabled;
}

FRAMEGEN_API bool IsEnabled() {
    return g_FrameGenConfig.enabled;
}

FRAMEGEN_API bool SetBackend(Backend backend) {
    if (!IsBackendSupported(backend)) {
        return false;
    }
    
    g_FrameGenConfig.backend = backend;
    
    // Recreate frame generator with new backend
    if (g_Hooks && g_Initialized) {
        g_FrameGenerator = FrameGen::CreateFrameGenerator(backend);
        if (g_FrameGenerator) {
            g_FrameGenerator->Initialize(
                g_Hooks->GetDevice(),
                g_Hooks->GetContext(),
                g_Hooks->GetSwapChain()
            );
        }
    }
    
    return true;
}

FRAMEGEN_API Backend GetBackend() {
    return g_FrameGenConfig.backend;
}

FRAMEGEN_API void SetQualityPreset(QualityPreset preset) {
    g_FrameGenConfig.quality = preset;
    SetD3D12Quality((int)preset);
    
    if (g_FrameGenerator) {
        g_FrameGenerator->SetQuality(preset);
    }
}

FRAMEGEN_API QualityPreset GetQualityPreset() {
    return g_FrameGenConfig.quality;
}

FRAMEGEN_API const Config& GetConfig() {
    return g_FrameGenConfig;
}

FRAMEGEN_API void SetConfig(const Config& config) {
    g_FrameGenConfig = config;
    
    if (g_FrameGenerator) {
        g_FrameGenerator->SetQuality(config.quality);
        g_FrameGenerator->SetSharpness(config.sharpness);
    }
}

FRAMEGEN_API const Stats& GetStats() {
    return g_Stats;
}

FRAMEGEN_API void ToggleOverlay() {
    if (g_Overlay) {
        g_Overlay->Toggle();
    }
}

FRAMEGEN_API bool IsBackendSupported(Backend backend) {
    switch (backend) {
        case Backend::None:
            return true;
            
        case Backend::FSR3:
            // FSR3 works on all modern GPUs
            return true;
            
        case Backend::DLSS3:
            // DLSS3 requires RTX 40 series
            if (g_Hooks && g_Hooks->GetDevice()) {
                // Check for RTX 40 series
                IDXGIDevice* dxgiDevice;
                g_Hooks->GetDevice()->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
                
                IDXGIAdapter* adapter;
                dxgiDevice->GetAdapter(&adapter);
                
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                
                adapter->Release();
                dxgiDevice->Release();
                
                // Check if NVIDIA and RTX 40 series (device IDs starting with 0x27)
                bool isNvidia = (desc.VendorId == 0x10DE);
                bool isRTX40 = (desc.DeviceId >= 0x2700 && desc.DeviceId < 0x2800);
                
                return isNvidia && isRTX40;
            }
            return false;
            
        case Backend::OpticalFlow:
            return true;
            
        default:
            return false;
    }
}

FRAMEGEN_API const char* GetLastError() {
    return g_LastError.c_str();
}

} // namespace FiveMFrameGen
