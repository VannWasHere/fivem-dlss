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
    FiveMFrameGen::Config g_FrameGenConfig;
    FiveMFrameGen::Stats g_Stats = {};
    
    // Error handling
    std::string g_LastError;
    
    // Hotkey for toggle overlay
    constexpr UINT OVERLAY_TOGGLE_KEY = VK_F10;
    constexpr UINT FRAMEGEN_TOGGLE_KEY = VK_F9;
}

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
    // Basic structured exception handling for crash diagnosis
    __try {
        InitializeModSafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Can't rely on our logger if it crashed, but try to output to debug string
        OutputDebugStringA("[FiveMFrameGen] FATAL EXCEPTION in InitializeMod\n");
    }
}

void InitializeModSafe() {
    try {
        FiveMFrameGen::Utils::Logger::Init("FiveMFrameGen.log");
        FiveMFrameGen::Utils::Logger::Info("FiveM Frame Generation Mod v%d.%d.%d starting...",
            FiveMFrameGen::VERSION_MAJOR,
            FiveMFrameGen::VERSION_MINOR,
            FiveMFrameGen::VERSION_PATCH
        );
        
        // Wait for game window
        FiveMFrameGen::Utils::Logger::Info("Waiting for FiveM window...");
        
        HWND gameWindow = nullptr;
        int attempts = 0;
        const int maxAttempts = 300; // 30 seconds max
        
        while (gameWindow == nullptr && attempts < maxAttempts) {
            gameWindow = FindFiveMWindow();
            if (gameWindow == nullptr) {
                Sleep(100);
                attempts++;
            }
        }
        
        if (gameWindow == nullptr) {
            FiveMFrameGen::Utils::Logger::Error("Failed to find FiveM window after %d attempts", attempts);
            g_LastError = "Could not find FiveM window";
            return;
        }
        
        FiveMFrameGen::Utils::Logger::Info("Found FiveM window (0x%p)", gameWindow);
        
        // Wait additional time for DirectX initialization
        Sleep(5000);
        
        // Load configuration
        g_Config = std::make_unique<FiveMFrameGen::Utils::ConfigManager>("FiveMFrameGen.ini");
        g_FrameGenConfig = g_Config->Load();
        
        // Initialize DirectX hooks
        FiveMFrameGen::Utils::Logger::Info("Initializing DirectX hooks...");
        
        g_Hooks = std::make_unique<FiveMFrameGen::Core::Hooks>();
        if (!g_Hooks->Initialize(gameWindow)) {
            FiveMFrameGen::Utils::Logger::Error("Failed to initialize DirectX hooks");
            g_LastError = "Failed to hook DirectX";
            return;
        }
        
        // Get D3D11 device from hooks
        ID3D11Device* device = g_Hooks->GetDevice();
        ID3D11DeviceContext* context = g_Hooks->GetContext();
        IDXGISwapChain* swapChain = g_Hooks->GetSwapChain();
        
        if (!device || !context || !swapChain) {
            FiveMFrameGen::Utils::Logger::Error("Failed to get D3D11 interfaces");
            g_LastError = "Failed to obtain D3D11 device";
            return;
        }
        
        // Initialize frame generator based on config
        FiveMFrameGen::Utils::Logger::Info("Initializing frame generator (Backend: %d)...",
            static_cast<int>(g_FrameGenConfig.backend));
        
        g_FrameGenerator = FiveMFrameGen::FrameGen::CreateFrameGenerator(g_FrameGenConfig.backend);
        if (!g_FrameGenerator) {
            FiveMFrameGen::Utils::Logger::Warn("Requested backend not available, falling back to FSR3");
            g_FrameGenerator = FiveMFrameGen::FrameGen::CreateFrameGenerator(FiveMFrameGen::Backend::FSR3);
        }
        
        if (!g_FrameGenerator || !g_FrameGenerator->Initialize(device, context, swapChain)) {
            FiveMFrameGen::Utils::Logger::Error("Failed to initialize frame generator");
            g_LastError = "Frame generator initialization failed";
            return;
        }
        
        // Initialize overlay
        FiveMFrameGen::Utils::Logger::Info("Initializing ImGui overlay...");
        
        g_Overlay = std::make_unique<FiveMFrameGen::Overlay::ImGuiOverlay>();
        if (!g_Overlay->Initialize(device, context, gameWindow)) {
            FiveMFrameGen::Utils::Logger::Warn("Failed to initialize overlay (non-critical)");
        }
        
        // Set up present callback
        g_Hooks->SetPresentCallback([](IDXGISwapChain* swapChain) {
            if (g_FrameGenerator && g_FrameGenConfig.enabled) {
                // Generate interpolated frame
                g_FrameGenerator->ProcessFrame();
                
                // Update stats
                g_Stats.baseFPS = g_FrameGenerator->GetBaseFPS();
                g_Stats.outputFPS = g_FrameGenerator->GetOutputFPS();
                g_Stats.frameTimeMs = g_FrameGenerator->GetFrameTimeMs();
                g_Stats.framesGenerated = g_FrameGenerator->GetFramesGenerated();
            }
            
            // Render overlay
            if (g_Overlay && g_FrameGenConfig.showOverlay) {
                g_Overlay->Render(g_FrameGenConfig, g_Stats);
            }
        });
        
        g_Initialized = true;
        FiveMFrameGen::Utils::Logger::Info("FiveM Frame Generation Mod initialized successfully!");
        
        // Log GPU info
        DXGI_ADAPTER_DESC adapterDesc;
        IDXGIDevice* dxgiDevice;
        IDXGIAdapter* adapter;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                adapter->GetDesc(&adapterDesc);
                
                char gpuName[128];
                size_t converted;
                wcstombs_s(&converted, gpuName, adapterDesc.Description, 128);
                FiveMFrameGen::Utils::Logger::Info("GPU: %s", gpuName);
                FiveMFrameGen::Utils::Logger::Info("VRAM: %zu MB", adapterDesc.DedicatedVideoMemory / (1024 * 1024));
                
                adapter->Release();
            }
            dxgiDevice->Release();
        }
    }
    catch (const std::exception& e) {
        FiveMFrameGen::Utils::Logger::Error("Exception in InitializeMod: %s", e.what());
    }
    catch (...) {
        FiveMFrameGen::Utils::Logger::Error("Unknown exception in InitializeMod");
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

/**
 * DLL entry point
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            
            // DEBUG: Prove we are loading
            MessageBoxA(NULL, "FiveM Frame Gen: Plugin Loaded!", "Debug", MB_OK | MB_ICONINFORMATION);
            
            // Start initialization in a separate thread
            std::thread(InitializeMod).detach();
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
