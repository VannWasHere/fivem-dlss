/**
 * ImGui Overlay Implementation - Modern UI Overhaul (D3D11 + D3D12)
 */

#include "imgui_overlay.h"
#include "../utils/logger.h"
#include <dxgi1_4.h>
#include <vector>
#include <algorithm>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h> // Ensure this exists

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// External D3D12 Quality Setter from main.cpp / hooks
extern void SetD3D12Quality(int quality); 

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

// D3D11 Initialization
bool ImGuiOverlay::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, HWND window) {
    if (m_Initialized) return true;
    m_IsD3D12 = false;
    
    m_Device11 = device;
    m_Context11 = context;
    m_Window = window;
    
    Utils::Logger::Info("Initializing ImGui Overlay (D3D11)...");
    DetectGPU();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    
    RECT r; 
    GetClientRect(window, &r);
    if ((r.right - r.left) > 2560) io.FontGlobalScale = 1.5f;

    ApplyStyle();
    
    if (!ImGui_ImplWin32_Init(window)) return false;
    if (!ImGui_ImplDX11_Init(device, context)) return false;
    
    m_OriginalWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    m_Initialized = true;
    return true;
}

// D3D12 Initialization
bool ImGuiOverlay::InitializeD3D12(ID3D12Device* device, int numFrames, DXGI_FORMAT rtvFormat, ID3D12CommandQueue* commandQueue, HWND window) {
    if (m_Initialized) return true;
    m_IsD3D12 = true;
    m_Device12 = device;
    m_Window = window;
    
    Utils::Logger::Info("Initializing ImGui Overlay (D3D12)...");
    DetectGPU();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    
    RECT r; 
    GetClientRect(window, &r);
    if ((r.right - r.left) > 2560) io.FontGlobalScale = 1.5f;

    ApplyStyle();
    
    if (!ImGui_ImplWin32_Init(window)) return false;
    
    // Create Descriptor Heap for Fonts
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_SrvDescHeap)))) {
        Utils::Logger::Error("Failed to create SRV Descriptor Heap for ImGui");
        return false;
    }
    
    if (!ImGui_ImplDX12_Init(device, numFrames, rtvFormat, m_SrvDescHeap.Get(), 
        m_SrvDescHeap->GetCPUDescriptorHandleForHeapStart(), 
        m_SrvDescHeap->GetGPUDescriptorHandleForHeapStart())) 
    {
        Utils::Logger::Error("ImGui_ImplDX12_Init failed");
        return false;
    }
    
    m_OriginalWndProc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
    m_Initialized = true;
    return true;
}

void ImGuiOverlay::DetectGPU() {
    m_GPUInfo = { "Unknown GPU", 0, false, false, false };
    
    // Prefer DX11 Device if available for simple query, or DX12
    IUnknown* device = m_IsD3D12 ? (IUnknown*)m_Device12.Get() : (IUnknown*)m_Device11;
    
    IDXGIDevice* dxgiDevice = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char name[128];
                size_t charsConverted = 0;
                wcstombs_s(&charsConverted, name, desc.Description, 128);
                m_GPUInfo.name = name;
                m_GPUInfo.vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
                m_GPUInfo.isNvidia = (desc.VendorId == 0x10DE);
                
                std::string sName = name;
                std::transform(sName.begin(), sName.end(), sName.begin(), ::toupper);
                if (m_GPUInfo.isNvidia && sName.find("RTX") != std::string::npos) {
                    m_GPUInfo.isRTX = true;
                }
                m_GPUInfo.isSupported = true; 
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }
}

void ImGuiOverlay::ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 8);
    
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.11f, 0.96f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.27f, 0.32f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    
    // Brand Colors (Electric Blue)
    ImVec4 accentNormal = ImVec4(0.00f, 0.48f, 1.00f, 0.90f);
    ImVec4 accentHover = ImVec4(0.10f, 0.55f, 1.00f, 1.00f);
    ImVec4 accentActive = ImVec4(0.00f, 0.40f, 0.90f, 1.00f);
    
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = accentNormal;
    colors[ImGuiCol_ButtonActive] = accentActive;
    
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    
    colors[ImGuiCol_SliderGrab] = accentNormal;
    colors[ImGuiCol_SliderGrabActive] = accentActive;
    colors[ImGuiCol_CheckMark] = accentNormal;
    
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    
    colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.30f, 0.50f);
    style.WindowBorderSize = 1.0f;
}

void ImGuiOverlay::Shutdown() {
    if (!m_Initialized) return;
    
    if (m_OriginalWndProc && m_Window) {
        SetWindowLongPtrW(m_Window, GWLP_WNDPROC, (LONG_PTR)m_OriginalWndProc);
        m_OriginalWndProc = nullptr;
    }
    
    if (m_IsD3D12) {
        ImGui_ImplDX12_Shutdown();
    } else {
        ImGui_ImplDX11_Shutdown();
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    m_Initialized = false;
}

void ImGuiOverlay::Toggle() {
    m_Visible = !m_Visible;
}

// D3D11 Render (Existing)
void ImGuiOverlay::Render(Config& config, const Stats& stats) {
    if (!m_Initialized || m_IsD3D12 || !m_Device11 || !m_Context11) return;
    
    if (m_RenderTargetView) {
        m_Context11->OMSetRenderTargets(1, &m_RenderTargetView, nullptr);
    }
    
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    if (m_Visible) {
        RenderConfigWindow(config, stats);
    }
    
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// D3D12 Render
void ImGuiOverlay::RenderD3D12(Config& config, const Stats& stats, ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    if (!m_Initialized || !m_IsD3D12 || !m_Device12) return;
    
    // Set Descriptor Heap (Required for Font)
    ID3D12DescriptorHeap* heaps[] = { m_SrvDescHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    if (m_Visible) {
        RenderConfigWindow(config, stats);
    }
    
    ImGui::Render();
    
    // Bind Render Target (Caller ensures it is in RenderTarget State)
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void ImGuiOverlay::RenderConfigWindow(Config& config, const Stats& stats) {
    // Reuse existing logic, it is API agnostic
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(450, 520), ImGuiCond_FirstUseEver);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;
    
    if (ImGui::Begin("FiveM Upscaler & FrameGen", &m_Visible, flags)) {
        
        ImGui::BeginChild("GPUCard", ImVec2(0, 100), true);
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "DETECTED GPU");
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", m_GPUInfo.name.c_str());
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::Text("%zu MB VRAM", m_GPUInfo.vramMB);
            ImGui::SameLine(ImGui::GetWindowWidth() - 110);
            if (m_GPUInfo.isSupported) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.1f, 1.0f));
                ImGui::Button("SUPPORTED", ImVec2(100, 20));
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.1f, 0.1f, 1.0f));
                ImGui::Button("UNSUPPORTED", ImVec2(100, 20));
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.0f, 0.48f, 1.00f, 1.0f), "UPSCALING SETTINGS");
        ImGui::Separator();
        ImGui::Spacing();
        
        bool enabled = config.enabled;
        if (enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.48f, 1.0f, 0.8f));
            if (ImGui::Button("ENABLED (Active)", ImVec2(-1, 40))) {
                config.enabled = false;
                SetD3D12Quality(0); 
            }
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("DISABLED (Click to Enable)", ImVec2(-1, 40))) {
                config.enabled = true;
            }
        }
        
        ImGui::Spacing();
        ImGui::Text("Render Quality:");
        const char* qualities[] = { "Performance (50%)", "Balanced (59%)", "Quality (67%)" };
        int q = (int)config.quality;
        if (q < 0 || q > 2) q = 1;
        
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##QualityCombo", &q, qualities, IM_ARRAYSIZE(qualities))) {
            config.quality = (QualityPreset)q;
            SetD3D12Quality(q);
        }
        
        ImGui::Spacing();
        ImGui::Text("Sharpness:");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##Sharp", &config.sharpness, 0.0f, 1.0f, "%.2f");
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "PERFORMANCE");
        
        m_FPSHistory[m_FPSHistoryIndex] = stats.outputFPS;
        m_FPSHistoryIndex = (m_FPSHistoryIndex + 1) % 60;
        
        char overlay[32];
        sprintf_s(overlay, "%.0f FPS", stats.outputFPS);
        ImGui::PlotLines("##Frames", m_FPSHistory, 60, m_FPSHistoryIndex, overlay, 0.0f, 200.0f, ImVec2(-1, 60));
        
        ImGui::Columns(2, "StatCols", false);
        ImGui::Text("Base: %.0f FPS", stats.baseFPS);
        ImGui::NextColumn();
        ImGui::Text("Gen: %.0f FPS", stats.outputFPS);
        ImGui::Columns(1);
        
        ImGui::Spacing();
        ImGui::TextDisabled("FiveM Upscaling Mod - v1.0.2 Public");
    }
    ImGui::End();
}

LRESULT CALLBACK ImGuiOverlay::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (s_Instance && s_Instance->m_Visible) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;
        
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
            if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MOUSEWHEEL)
                return true;
        }
    }
    
    if (msg == WM_KEYDOWN && wParam == VK_F10) {
        if (s_Instance) s_Instance->Toggle();
    }
    
    if (s_Instance && s_Instance->m_OriginalWndProc)
        return CallWindowProcW(s_Instance->m_OriginalWndProc, hWnd, msg, wParam, lParam);
        
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace Overlay
} // namespace FiveMFrameGen
