/**
 * ImGui Overlay Implementation
 */

#include "imgui_overlay.h"
#include "../utils/logger.h"

// ImGui includes
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace FiveMFrameGen {
namespace Overlay {

ImGuiOverlay* ImGuiOverlay::s_Instance = nullptr;

ImGuiOverlay::ImGuiOverlay() {
    s_Instance = this;
}

ImGuiOverlay::~ImGuiOverlay() {
    Shutdown();
    s_Instance = nullptr;
}

bool ImGuiOverlay::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, HWND window) {
    if (m_Initialized) return true;
    
    m_Device = device;
    m_Context = context;
    m_Window = window;
    
    Utils::Logger::Info("Initializing ImGui overlay...");
    
    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Don't save ini file
    
    // Setup style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 0.95f;
    style.FrameRounding = 4.0f;
    style.WindowRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    
    // Custom colors with blue accent
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.95f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.35f, 0.60f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.45f, 0.70f, 0.90f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.35f, 0.60f, 0.80f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.45f, 0.70f, 0.90f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.18f, 0.80f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.25f, 0.90f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.20f, 0.30f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    
    // Initialize platform/renderer
    if (!ImGui_ImplWin32_Init(window)) {
        Utils::Logger::Error("Failed to initialize ImGui Win32");
        ImGui::DestroyContext();
        return false;
    }
    
    if (!ImGui_ImplDX11_Init(device, context)) {
        Utils::Logger::Error("Failed to initialize ImGui DX11");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    
    // Hook window procedure for input
    m_OriginalWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    
    m_Initialized = true;
    Utils::Logger::Info("ImGui overlay initialized");
    
    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!m_Initialized) return;
    
    Utils::Logger::Info("Shutting down ImGui overlay...");
    
    // Restore window procedure
    if (m_OriginalWndProc && m_Window) {
        SetWindowLongPtrW(m_Window, GWLP_WNDPROC, (LONG_PTR)m_OriginalWndProc);
        m_OriginalWndProc = nullptr;
    }
    
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    m_Initialized = false;
}

void ImGuiOverlay::Toggle() {
    m_Visible = !m_Visible;
    Utils::Logger::Info("Overlay %s", m_Visible ? "shown" : "hidden");
}

void ImGuiOverlay::Render(Config& config, const Stats& stats) {
    if (!m_Initialized || !m_Device || !m_Context) return;
    
    // Get the back buffer and create render target if needed
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    
    // Get swap chain from device
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(m_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    
    // Create render target from back buffer if we don't have one
    if (!m_RenderTargetView) {
        // We need to get the swap chain - try getting it from the window
        RECT rect;
        GetClientRect(m_Window, &rect);
        
        // Create a simple render target
        ID3D11Texture2D* backBuffer = nullptr;
        
        // For now, just proceed without render target - ImGui should still work
    }
    
    // Set render target if we have one
    if (m_RenderTargetView) {
        m_Context->OMSetRenderTargets(1, &m_RenderTargetView, nullptr);
    }
    
    // Start new frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    // Always show a small performance indicator in corner
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(150, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.5f);
    
    ImGuiWindowFlags indicatorFlags = 
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs;
    
    if (ImGui::Begin("##FPSIndicator", nullptr, indicatorFlags)) {
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Frame Gen: %s", 
            config.enabled ? "ON" : "OFF");
        ImGui::Text("FPS: %.1f -> %.1f", stats.baseFPS, stats.outputFPS);
        ImGui::Text("F10: Settings");
    }
    ImGui::End();
    
    // Render config window if visible
    if (m_Visible) {
        RenderConfigWindow(config, stats);
    }
    
    // Render ImGui
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void ImGuiOverlay::RenderConfigWindow(Config& config, const Stats& stats) {
    ImGui::SetNextWindowPos(ImVec2(50, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_FirstUseEver);
    
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse;
    
    if (ImGui::Begin("FiveM Frame Generation Settings", &m_Visible, windowFlags)) {
        // Header
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Frame Generation Mod v1.0.0");
        ImGui::Separator();
        ImGui::Spacing();
        
        // Enable toggle
        if (ImGui::Checkbox("Enable Frame Generation", &config.enabled)) {
            Utils::Logger::Info("Frame generation %s via UI", 
                config.enabled ? "enabled" : "disabled");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(F9)");
        
        ImGui::Spacing();
        
        // Backend selection
        ImGui::Text("Backend:");
        const char* backends[] = { "None", "FSR 3 (All GPUs)", "DLSS 3 (RTX 40 only)", "Optical Flow" };
        int backend = static_cast<int>(config.backend);
        if (ImGui::Combo("##Backend", &backend, backends, IM_ARRAYSIZE(backends))) {
            config.backend = static_cast<Backend>(backend);
        }
        
        ImGui::Spacing();
        
        // Quality preset
        ImGui::Text("Quality:");
        const char* quality[] = { "Performance", "Balanced", "Quality" };
        int qualityPreset = static_cast<int>(config.quality);
        if (ImGui::Combo("##Quality", &qualityPreset, quality, IM_ARRAYSIZE(quality))) {
            config.quality = static_cast<QualityPreset>(qualityPreset);
        }
        
        ImGui::Spacing();
        
        // Sharpness slider
        ImGui::Text("Sharpness:");
        ImGui::SliderFloat("##Sharpness", &config.sharpness, 0.0f, 1.0f, "%.2f");
        
        ImGui::Spacing();
        
        // HUD mode
        ImGui::Checkbox("HUD-less Mode", &config.hudLessMode);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Excludes HUD from interpolation\nto reduce UI artifacts");
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Performance stats
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Performance:");
        
        ImGui::Columns(2, nullptr, false);
        
        ImGui::Text("Base FPS:");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "%.1f", stats.baseFPS);
        ImGui::NextColumn();
        
        ImGui::Text("Output FPS:");
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%.1f", stats.outputFPS);
        ImGui::NextColumn();
        
        ImGui::Text("Frame Time:");
        ImGui::NextColumn();
        ImGui::Text("%.2f ms", stats.frameTimeMs);
        ImGui::NextColumn();
        
        ImGui::Text("Frames Generated:");
        ImGui::NextColumn();
        ImGui::Text("%llu", stats.framesGenerated);
        ImGui::NextColumn();
        
        ImGui::Columns(1);
        
        ImGui::Spacing();
        
        // FPS Graph
        RenderPerformanceGraph(stats);
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Help
        ImGui::TextDisabled("Hotkeys:");
        ImGui::TextDisabled("  F9  - Toggle Frame Generation");
        ImGui::TextDisabled("  F10 - Toggle This Menu");
    }
    ImGui::End();
}

void ImGuiOverlay::RenderPerformanceGraph(const Stats& stats) {
    // Update history
    m_FPSHistory[m_FPSHistoryIndex] = stats.outputFPS;
    m_FPSHistoryIndex = (m_FPSHistoryIndex + 1) % 120;
    
    // Find min/max for scaling
    float minFPS = 1000.0f;
    float maxFPS = 0.0f;
    for (int i = 0; i < 120; i++) {
        if (m_FPSHistory[i] > 0) {
            minFPS = (std::min)(minFPS, m_FPSHistory[i]);
            maxFPS = (std::max)(maxFPS, m_FPSHistory[i]);
        }
    }
    
    // Add padding
    float range = maxFPS - minFPS;
    if (range < 10.0f) range = 10.0f;
    minFPS = (std::max)(0.0f, minFPS - range * 0.1f);
    maxFPS = maxFPS + range * 0.1f;
    
    // Plot
    char overlay[32];
    snprintf(overlay, sizeof(overlay), "%.0f FPS", stats.outputFPS);
    ImGui::PlotLines("##FPSGraph", m_FPSHistory, 120, m_FPSHistoryIndex,
        overlay, minFPS, maxFPS, ImVec2(0, 60));
}

LRESULT CALLBACK ImGuiOverlay::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Handle toggle key
    if (msg == WM_KEYDOWN && wParam == VK_F10) {
        if (s_Instance) {
            s_Instance->Toggle();
        }
    }
    
    // Let ImGui handle input when visible
    if (s_Instance && s_Instance->m_Visible) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            return true;
        }
    }
    
    // Call original
    if (s_Instance && s_Instance->m_OriginalWndProc) {
        return CallWindowProcW(s_Instance->m_OriginalWndProc, hWnd, msg, wParam, lParam);
    }
    
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace Overlay
} // namespace FiveMFrameGen
