#pragma once

/**
 * DirectX 11 Hooking System
 * 
 * This module handles hooking into the DirectX 11 rendering pipeline
 * to intercept frames for frame generation.
 */

#ifndef FIVEM_FRAMEGEN_HOOKS_H
#define FIVEM_FRAMEGEN_HOOKS_H

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <functional>
#include <memory>

namespace FiveMFrameGen {
namespace Core {

/**
 * Callback type for present hook
 */
using PresentCallback = std::function<void(IDXGISwapChain*)>;

/**
 * DirectX 11 Hooks Manager
 * 
 * Manages hooks into the D3D11 rendering pipeline for frame interception.
 */
class Hooks {
public:
    Hooks();
    ~Hooks();
    
    // Non-copyable
    Hooks(const Hooks&) = delete;
    Hooks& operator=(const Hooks&) = delete;
    
    /**
     * Initialize hooks for the given window
     * 
     * @param gameWindow The game's main window handle
     * @return True if initialization succeeded
     */
    bool Initialize(HWND gameWindow);
    
    /**
     * Shutdown and restore original functions
     */
    void Shutdown();
    
    /**
     * Check if hooks are active
     */
    bool IsInitialized() const { return m_Initialized; }
    
    /**
     * Set callback to be called on each present
     */
    void SetPresentCallback(PresentCallback callback);
    
    /**
     * Get the D3D11 device
     */
    ID3D11Device* GetDevice() const { return m_Device; }
    
    /**
     * Get the D3D11 device context
     */
    ID3D11DeviceContext* GetContext() const { return m_Context; }
    
    /**
     * Get the DXGI swap chain
     */
    IDXGISwapChain* GetSwapChain() const { return m_SwapChain; }
    
    /**
     * Get the render target view
     */
    ID3D11RenderTargetView* GetRenderTargetView() const { return m_RenderTargetView; }

private:
    /**
     * Create a dummy device to get vtable addresses
     */
    bool GetD3D11VTable(void** vtable, size_t size);
    
    /**
     * Hook the present function
     */
    bool HookPresent(void* presentFunc);
    
    /**
     * Hook resize buffers function
     */
    bool HookResizeBuffers(void* resizeFunc);
    
    /**
     * Create render target view from swap chain
     */
    bool CreateRenderTarget();
    
    /**
     * Release render target view
     */
    void ReleaseRenderTarget();
    
    // Hook callback - called before original present
    static HRESULT STDMETHODCALLTYPE HookedPresent(
        IDXGISwapChain* pSwapChain,
        UINT SyncInterval,
        UINT Flags
    );
    
    // Hook callback - called when buffers are resized
    static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
        IDXGISwapChain* pSwapChain,
        UINT BufferCount,
        UINT Width,
        UINT Height,
        DXGI_FORMAT NewFormat,
        UINT SwapChainFlags
    );

private:
    bool m_Initialized = false;
    
    // D3D11 interfaces
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    IDXGISwapChain* m_SwapChain = nullptr;
    ID3D11RenderTargetView* m_RenderTargetView = nullptr;
    
    // Game window
    HWND m_GameWindow = nullptr;
    
    // Callbacks
    PresentCallback m_PresentCallback;
    
    // Original function pointers
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    
    static PresentFn s_OriginalPresent;
    static ResizeBuffersFn s_OriginalResizeBuffers;
    static Hooks* s_Instance;
};

/**
 * DXGI SwapChain VTable indices
 */
namespace SwapChainVTable {
    constexpr size_t QueryInterface = 0;
    constexpr size_t AddRef = 1;
    constexpr size_t Release = 2;
    constexpr size_t SetPrivateData = 3;
    constexpr size_t SetPrivateDataInterface = 4;
    constexpr size_t GetPrivateData = 5;
    constexpr size_t GetParent = 6;
    constexpr size_t GetDevice = 7;
    constexpr size_t Present = 8;
    constexpr size_t GetBuffer = 9;
    constexpr size_t SetFullscreenState = 10;
    constexpr size_t GetFullscreenState = 11;
    constexpr size_t GetDesc = 12;
    constexpr size_t ResizeBuffers = 13;
    constexpr size_t ResizeTarget = 14;
    constexpr size_t GetContainingOutput = 15;
    constexpr size_t GetFrameStatistics = 16;
    constexpr size_t GetLastPresentCount = 17;
    
    constexpr size_t VTABLE_SIZE = 18;
}

} // namespace Core
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_HOOKS_H
