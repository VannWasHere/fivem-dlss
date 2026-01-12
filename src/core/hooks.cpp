/**
 * DirectX 11 Hooking Implementation
 * 
 * Uses MinHook to intercept D3D11 Present calls for frame generation.
 */

#include "hooks.h"
#include "../utils/logger.h"

#include <MinHook.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace Core {

// Static member initialization
Hooks::PresentFn Hooks::s_OriginalPresent = nullptr;
Hooks::ResizeBuffersFn Hooks::s_OriginalResizeBuffers = nullptr;
Hooks* Hooks::s_Instance = nullptr;

Hooks::Hooks() {
    s_Instance = this;
}

Hooks::~Hooks() {
    Shutdown();
    s_Instance = nullptr;
}

bool Hooks::Initialize(HWND gameWindow) {
    if (m_Initialized) {
        Utils::Logger::Warn("Hooks already initialized");
        return true;
    }
    
    m_GameWindow = gameWindow;
    
    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Utils::Logger::Error("Failed to initialize MinHook: %d", status);
        return false;
    }
    
    // Get D3D11 vtable
    void* vtable[SwapChainVTable::VTABLE_SIZE];
    if (!GetD3D11VTable(vtable, SwapChainVTable::VTABLE_SIZE)) {
        Utils::Logger::Error("Failed to get D3D11 vtable");
        return false;
    }
    
    // Hook Present
    if (!HookPresent(vtable[SwapChainVTable::Present])) {
        Utils::Logger::Error("Failed to hook Present");
        return false;
    }
    
    // Hook ResizeBuffers
    if (!HookResizeBuffers(vtable[SwapChainVTable::ResizeBuffers])) {
        Utils::Logger::Error("Failed to hook ResizeBuffers");
        return false;
    }
    
    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        Utils::Logger::Error("Failed to enable hooks: %d", status);
        return false;
    }
    
    m_Initialized = true;
    Utils::Logger::Info("DirectX hooks initialized successfully");
    
    return true;
}

void Hooks::Shutdown() {
    if (!m_Initialized) return;
    
    Utils::Logger::Info("Shutting down DirectX hooks...");
    
    // Disable all hooks
    MH_DisableHook(MH_ALL_HOOKS);
    
    // Remove hooks
    MH_RemoveHook(MH_ALL_HOOKS);
    
    // Uninitialize MinHook
    MH_Uninitialize();
    
    // Release render target
    ReleaseRenderTarget();
    
    // Release D3D11 interfaces
    if (m_SwapChain) {
        m_SwapChain->Release();
        m_SwapChain = nullptr;
    }
    if (m_Context) {
        m_Context->Release();
        m_Context = nullptr;
    }
    if (m_Device) {
        m_Device->Release();
        m_Device = nullptr;
    }
    
    m_Initialized = false;
}

void Hooks::SetPresentCallback(PresentCallback callback) {
    m_PresentCallback = std::move(callback);
}

bool Hooks::GetD3D11VTable(void** vtable, size_t size) {
    // Create a dummy swap chain to get the vtable
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 2;
    sd.BufferDesc.Height = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_GameWindow;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    D3D_FEATURE_LEVEL featureLevel;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &sd,
        &swapChain,
        &device,
        &featureLevel,
        &context
    );
    
    if (FAILED(hr)) {
        // Try with NULL feature level
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &sd,
            &swapChain,
            &device,
            &featureLevel,
            &context
        );
        
        if (FAILED(hr)) {
            Utils::Logger::Error("Failed to create D3D11 device: 0x%08X", hr);
            return false;
        }
    }
    
    // Copy vtable
    void** swapChainVtable = *reinterpret_cast<void***>(swapChain);
    memcpy(vtable, swapChainVtable, size * sizeof(void*));
    
    // Cleanup
    swapChain->Release();
    context->Release();
    device->Release();
    
    Utils::Logger::Info("Got D3D11 vtable (Feature Level: 0x%X)", featureLevel);
    
    return true;
}

bool Hooks::HookPresent(void* presentFunc) {
    void* originalPresent = nullptr;
    
    MH_STATUS status = MH_CreateHook(
        presentFunc,
        reinterpret_cast<void*>(&HookedPresent),
        &originalPresent
    );
    
    if (status != MH_OK) {
        Utils::Logger::Error("MH_CreateHook(Present) failed: %d", status);
        return false;
    }
    
    s_OriginalPresent = reinterpret_cast<PresentFn>(originalPresent);
    Utils::Logger::Info("Hooked Present at %p", presentFunc);
    
    return true;
}

bool Hooks::HookResizeBuffers(void* resizeFunc) {
    void* originalResize = nullptr;
    
    MH_STATUS status = MH_CreateHook(
        resizeFunc,
        reinterpret_cast<void*>(&HookedResizeBuffers),
        &originalResize
    );
    
    if (status != MH_OK) {
        Utils::Logger::Error("MH_CreateHook(ResizeBuffers) failed: %d", status);
        return false;
    }
    
    s_OriginalResizeBuffers = reinterpret_cast<ResizeBuffersFn>(originalResize);
    Utils::Logger::Info("Hooked ResizeBuffers at %p", resizeFunc);
    
    return true;
}

bool Hooks::CreateRenderTarget() {
    if (!m_SwapChain || !m_Device) return false;
    
    // Get back buffer
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to get back buffer: 0x%08X", hr);
        return false;
    }
    
    // Create render target view
    hr = m_Device->CreateRenderTargetView(backBuffer, nullptr, &m_RenderTargetView);
    backBuffer->Release();
    
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create RTV: 0x%08X", hr);
        return false;
    }
    
    return true;
}

void Hooks::ReleaseRenderTarget() {
    if (m_RenderTargetView) {
        m_RenderTargetView->Release();
        m_RenderTargetView = nullptr;
    }
}

HRESULT STDMETHODCALLTYPE Hooks::HookedPresent(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags
) {
    if (s_Instance && !s_Instance->m_Device) {
        // First call - get device from swap chain
        pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&s_Instance->m_Device);
        if (s_Instance->m_Device) {
            s_Instance->m_Device->GetImmediateContext(&s_Instance->m_Context);
            s_Instance->m_SwapChain = pSwapChain;
            pSwapChain->AddRef();
            
            s_Instance->CreateRenderTarget();
            
            Utils::Logger::Info("Captured D3D11 device from Present hook");
        }
    }
    
    // Call callback if set
    if (s_Instance && s_Instance->m_PresentCallback) {
        s_Instance->m_PresentCallback(pSwapChain);
    }
    
    // Call original
    return s_OriginalPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE Hooks::HookedResizeBuffers(
    IDXGISwapChain* pSwapChain,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags
) {
    // Release render target before resize
    if (s_Instance) {
        s_Instance->ReleaseRenderTarget();
    }
    
    // Call original
    HRESULT hr = s_OriginalResizeBuffers(
        pSwapChain,
        BufferCount,
        Width,
        Height,
        NewFormat,
        SwapChainFlags
    );
    
    // Recreate render target after resize
    if (SUCCEEDED(hr) && s_Instance) {
        s_Instance->CreateRenderTarget();
        Utils::Logger::Info("Resize buffers: %dx%d", Width, Height);
    }
    
    return hr;
}

} // namespace Core
} // namespace FiveMFrameGen
