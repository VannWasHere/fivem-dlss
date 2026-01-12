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
    
    // Output FPS with frame gen
    constexpr UINT OVERLAY_TOGGLE_KEY = VK_F10;
    constexpr UINT FRAMEGEN_TOGGLE_KEY = VK_F9;
}

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

// Overlay window class and procedure
static HWND g_SettingsWindow = nullptr;
static const wchar_t* SETTINGS_CLASS = L"FrameGenSettings";

// Button IDs
#define BTN_TOGGLE_FRAMEGEN 101
#define BTN_QUALITY_LEFT    102
#define BTN_QUALITY_RIGHT   103
#define BTN_CLOSE           104

// GPU Info
struct GPUInfo {
    char name[256] = "Unknown GPU";
    size_t vramMB = 0;
    bool isNvidia = false;
    bool isRTX40 = false;
    bool dlssSupported = false;
};
static GPUInfo g_GPUInfo;

void DetectGPU() {
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                size_t converted;
                wcstombs_s(&converted, g_GPUInfo.name, desc.Description, 255);
                g_GPUInfo.vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
                g_GPUInfo.isNvidia = (desc.VendorId == 0x10DE);
                
                if (g_GPUInfo.isNvidia) {
                    if (strstr(g_GPUInfo.name, "RTX 40") || strstr(g_GPUInfo.name, "RTX 4")) {
                        g_GPUInfo.isRTX40 = true;
                        g_GPUInfo.dlssSupported = true;
                    } else if (strstr(g_GPUInfo.name, "RTX 3") || strstr(g_GPUInfo.name, "RTX 2")) {
                        g_GPUInfo.dlssSupported = true;
                    }
                }
            }
            adapter->Release();
        }
        factory->Release();
    }
    LogRaw("GPU Detected: %s (%zu MB)", g_GPUInfo.name, g_GPUInfo.vramMB);
}

void DrawButton(HDC hdc, int x, int y, int w, int h, const char* text, bool highlight) {
    HBRUSH brush = CreateSolidBrush(highlight ? RGB(60, 120, 200) : RGB(50, 50, 70));
    RECT r = { x, y, x + w, y + h };
    FillRect(hdc, &r, brush);
    DeleteObject(brush);
    
    // Border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(100, 100, 120));
    SelectObject(hdc, pen);
    MoveToEx(hdc, x, y, NULL); LineTo(hdc, x + w, y);
    LineTo(hdc, x + w, y + h); LineTo(hdc, x, y + h); LineTo(hdc, x, y);
    DeleteObject(pen);
    
    // Text
    SetTextColor(hdc, RGB(220, 220, 220));
    RECT tr = { x, y, x + w, y + h };
    DrawTextA(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HFONT titleFont = nullptr;
    static HFONT textFont = nullptr;
    static HDC memDC = nullptr;
    static HBITMAP memBitmap = nullptr;
    
    switch (msg) {
        case WM_CREATE: {
            titleFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
            textFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
            return 0;
        }
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int width = clientRect.right;
            int height = clientRect.bottom;
            
            // Create memory DC for double buffering (no flicker)
            if (!memDC) {
                memDC = CreateCompatibleDC(hdc);
                memBitmap = CreateCompatibleBitmap(hdc, width, height);
                SelectObject(memDC, memBitmap);
            }
            
            // Draw to memory DC
            HDC dc = memDC;
            
            // Background
            HBRUSH bgBrush = CreateSolidBrush(RGB(20, 22, 30));
            FillRect(dc, &clientRect, bgBrush);
            DeleteObject(bgBrush);
            
            SetBkMode(dc, TRANSPARENT);
            int y = 12;
            
            // Title bar background
            RECT titleBar = { 0, 0, width, 40 };
            HBRUSH titleBrush = CreateSolidBrush(RGB(30, 35, 50));
            FillRect(dc, &titleBar, titleBrush);
            DeleteObject(titleBrush);
            
            // Title
            SelectObject(dc, titleFont);
            SetTextColor(dc, RGB(100, 180, 255));
            TextOutA(dc, 15, 10, "Frame Generation Settings", 26);
            
            // Close button
            DrawButton(dc, width - 35, 5, 30, 30, "X", false);
            
            y = 50;
            
            // GPU Section
            SelectObject(dc, textFont);
            SetTextColor(dc, RGB(120, 120, 140));
            TextOutA(dc, 15, y, "GPU", 3);
            y += 22;
            
            SetTextColor(dc, RGB(200, 200, 200));
            TextOutA(dc, 15, y, g_GPUInfo.name, (int)strlen(g_GPUInfo.name));
            y += 20;
            
            char vramText[64];
            sprintf_s(vramText, "VRAM: %zu MB", g_GPUInfo.vramMB);
            SetTextColor(dc, RGB(150, 150, 150));
            TextOutA(dc, 15, y, vramText, (int)strlen(vramText));
            y += 25;
            
            // DLSS Support
            if (g_GPUInfo.isRTX40) {
                SetTextColor(dc, RGB(100, 255, 100));
                TextOutA(dc, 15, y, "DLSS 3 Frame Gen: Supported", 28);
            } else if (g_GPUInfo.dlssSupported) {
                SetTextColor(dc, RGB(255, 200, 100));
                TextOutA(dc, 15, y, "DLSS: Yes | Frame Gen: Requires RTX 40", 39);
            } else {
                SetTextColor(dc, RGB(255, 150, 100));
                TextOutA(dc, 15, y, "Using FSR3 Fallback", 19);
            }
            y += 35;
            
            // Separator
            HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(50, 55, 70));
            SelectObject(dc, sepPen);
            MoveToEx(dc, 15, y, NULL); LineTo(dc, width - 15, y);
            DeleteObject(sepPen);
            y += 15;
            
            // Frame Gen Toggle Section
            SetTextColor(dc, RGB(120, 120, 140));
            TextOutA(dc, 15, y, "FRAME GENERATION", 16);
            y += 25;
            
            // Toggle Button
            DrawButton(dc, 15, y, 150, 35, 
                g_FrameGenConfig.enabled ? "ENABLED" : "DISABLED", 
                g_FrameGenConfig.enabled);
            y += 50;
            
            // Quality Section
            SetTextColor(dc, RGB(120, 120, 140));
            TextOutA(dc, 15, y, "QUALITY PRESET", 14);
            y += 25;
            
            // Quality selector with arrows
            const char* qualityNames[] = { "Performance", "Balanced", "Quality", "Ultra Quality" };
            int q = static_cast<int>(g_FrameGenConfig.quality);
            if (q < 0 || q > 3) q = 1;
            
            DrawButton(dc, 15, y, 30, 30, "<", false);
            
            // Quality display box
            RECT qRect = { 50, y, 220, y + 30 };
            HBRUSH qBrush = CreateSolidBrush(RGB(40, 45, 60));
            FillRect(dc, &qRect, qBrush);
            DeleteObject(qBrush);
            SetTextColor(dc, RGB(100, 180, 255));
            DrawTextA(dc, qualityNames[q], -1, &qRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            DrawButton(dc, 225, y, 30, 30, ">", false);
            y += 45;
            
            // Hotkeys info
            SetTextColor(dc, RGB(100, 100, 120));
            TextOutA(dc, 15, y, "Hotkeys: F9 Toggle | F10 Window | F7 Quality", 45);
            
            // Copy memory DC to screen (no flicker)
            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            RECT r;
            GetClientRect(hwnd, &r);
            
            // Close button
            if (x >= r.right - 35 && x <= r.right - 5 && y >= 5 && y <= 35) {
                g_FrameGenConfig.showOverlay = false;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            
            // Frame Gen toggle (approx y=175)
            if (x >= 15 && x <= 165 && y >= 175 && y <= 210) {
                g_FrameGenConfig.enabled = !g_FrameGenConfig.enabled;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            
            // Quality left arrow (approx y=245)
            if (x >= 15 && x <= 45 && y >= 245 && y <= 275) {
                int q = static_cast<int>(g_FrameGenConfig.quality);
                q = (q - 1 + 4) % 4;
                g_FrameGenConfig.quality = static_cast<FiveMFrameGen::QualityPreset>(q);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            
            // Quality right arrow
            if (x >= 225 && x <= 255 && y >= 245 && y <= 275) {
                int q = static_cast<int>(g_FrameGenConfig.quality);
                q = (q + 1) % 4;
                g_FrameGenConfig.quality = static_cast<FiveMFrameGen::QualityPreset>(q);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            
            // Allow dragging title bar
            if (y < 40) {
                ReleaseCapture();
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }
        
        case WM_DESTROY:
            if (memDC) { DeleteDC(memDC); memDC = nullptr; }
            if (memBitmap) { DeleteObject(memBitmap); memBitmap = nullptr; }
            if (titleFont) { DeleteObject(titleFont); titleFont = nullptr; }
            if (textFont) { DeleteObject(textFont); textFont = nullptr; }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateSettingsWindow(HWND gameWindow) {
    DetectGPU();
    
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = SETTINGS_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_OWNDC; // For double buffering
    RegisterClassExW(&wc);
    
    RECT gameRect;
    GetWindowRect(gameWindow, &gameRect);
    
    g_SettingsWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SETTINGS_CLASS,
        L"",
        WS_POPUP,
        gameRect.left + 50, gameRect.top + 50,
        280, 320,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    
    if (g_SettingsWindow) {
        ShowWindow(g_SettingsWindow, SW_SHOWNOACTIVATE);
        LogRaw("Settings window created!");
    }
}

void InputLoop() {
    LogRaw("Starting InputLoop...");
    
    HWND gameWindow = FindFiveMWindow();
    if (gameWindow) {
        CreateSettingsWindow(gameWindow);
    }
    
    while (g_Initialized) {
        if (g_SettingsWindow) {
            MSG msg;
            while (PeekMessageW(&msg, g_SettingsWindow, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        
        // F9 - Toggle Frame Gen
        if (GetAsyncKeyState(VK_F9) & 1) {
            g_FrameGenConfig.enabled = !g_FrameGenConfig.enabled;
            if (g_SettingsWindow) InvalidateRect(g_SettingsWindow, NULL, FALSE);
        }
        
        // F10 - Toggle Window
        if (GetAsyncKeyState(VK_F10) & 1) {
            g_FrameGenConfig.showOverlay = !g_FrameGenConfig.showOverlay;
            if (g_SettingsWindow) {
                ShowWindow(g_SettingsWindow, g_FrameGenConfig.showOverlay ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
        }
        
        // F7 - Cycle Quality
        if (GetAsyncKeyState(VK_F7) & 1) {
            int q = static_cast<int>(g_FrameGenConfig.quality);
            q = (q + 1) % 4;
            g_FrameGenConfig.quality = static_cast<FiveMFrameGen::QualityPreset>(q);
            if (g_SettingsWindow) InvalidateRect(g_SettingsWindow, NULL, FALSE);
        }
        
        Sleep(16);
    }
    
    if (g_SettingsWindow) DestroyWindow(g_SettingsWindow);
    LogRaw("InputLoop exited");
}

void InitializeModSafe() {
    LogRaw("InitializeModSafe: Step 1 - Entry");
    
    try {
        // Skip complex initialization - just enable the overlay for testing
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
        LogRaw("InitializeModSafe: Step 3 - Window found: 0x%p", gameWindow);
        
        // Check if D3D12 is being used (FiveM uses D3D12!)
        bool useD3D12 = GetModuleHandleA("d3d12.dll") != nullptr;
        LogRaw("InitializeModSafe: Step 4 - D3D12 detected: %s", useD3D12 ? "YES" : "NO");
        
        // Wait for DX
        LogRaw("InitializeModSafe: Step 5 - Waiting 3s for DirectX");
        Sleep(3000);
        
        if (useD3D12) {
            // FiveM uses D3D12 - use D3D12 hooks
            LogRaw("InitializeModSafe: Step 6 - Using D3D12 hooks for FiveM");
            
            // Use MinHook to detect - D3D12 init function is in hooks_d3d12.cpp
            bool InitD3D12Hooks(HWND);
            
            if (!InitD3D12Hooks(gameWindow)) {
                LogRaw("ERROR: D3D12 Hooks Init Failed - falling back to D3D11");
                useD3D12 = false;
            } else {
                LogRaw("InitializeModSafe: D3D12 hooks installed successfully!");
                g_Initialized = true;
                LogRaw("InitializeModSafe: SUCCESS! Starting InputLoop (D3D12 mode)");
                InputLoop();
                return;
            }
        }
        
        // Fallback to D3D11 if D3D12 not available
        LogRaw("InitializeModSafe: Step 6 - Creating D3D11 Hooks");
        g_Hooks = std::make_unique<FiveMFrameGen::Core::Hooks>();
        
        LogRaw("InitializeModSafe: Step 7 - Initializing D3D11 Hooks");
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
