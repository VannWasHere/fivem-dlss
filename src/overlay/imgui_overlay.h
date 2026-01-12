#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "../include/fivem_framegen.h"

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace Overlay {

struct GPUInfo {
    std::string name;
    size_t vramMB;
    bool isNvidia;
    bool isRTX;
    bool isSupported;
};

class ImGuiOverlay {
public:
    ImGuiOverlay();
    ~ImGuiOverlay();

    // D3D11 Init
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, HWND window);
    
    // D3D12 Init
    bool InitializeD3D12(ID3D12Device* device, int numFrames, DXGI_FORMAT rtvFormat, ID3D12CommandQueue* commandQueue, HWND window);

    void Shutdown();
    void Toggle();
    bool IsVisible() const { return m_Visible; }
    
    // Config & Stats are updated here
    // For D3D11, RTV is needed. For D3D12, RTV Handle is needed (in descriptor heap).
    
    void Render(Config& config, const Stats& stats); // D3D11
    void RenderD3D12(Config& config, const Stats& stats, ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

    void SetRenderTarget(ID3D11RenderTargetView* rtv) { m_RenderTargetView = rtv; }

private:
    void RenderConfigWindow(Config& config, const Stats& stats);
    void DetectGPU();
    void ApplyStyle();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool m_Initialized = false;
    bool m_Visible = false;
    bool m_IsD3D12 = false;
    
    HWND m_Window = nullptr;
    WNDPROC m_OriginalWndProc = nullptr;
    
    // D3D11 Resources
    ID3D11Device* m_Device11 = nullptr;
    ID3D11DeviceContext* m_Context11 = nullptr;
    ID3D11RenderTargetView* m_RenderTargetView = nullptr;
    
    // D3D12 Resources
    ComPtr<ID3D12Device> m_Device12;
    ComPtr<ID3D12DescriptorHeap> m_SrvDescHeap;
    
    GPUInfo m_GPUInfo;
    
    // Graph
    float m_FPSHistory[60] = {};
    int m_FPSHistoryIndex = 0;
    
    static ImGuiOverlay* s_Instance;
};

} // namespace Overlay
} // namespace FiveMFrameGen
