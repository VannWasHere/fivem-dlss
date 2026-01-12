/**
 * DirectX 12 Hooking Implementation for FiveM
 * 
 * FiveM uses D3D12, not D3D11. This hooks ExecuteCommandLists.
 */

#include "hooks.h"
#include "../utils/logger.h"

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace Core {

// D3D12 types
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*,
    UINT,
    ID3D12CommandList* const*
);

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain*,
    UINT,
    UINT
);

// Static members
static ExecuteCommandListsFn s_OriginalExecuteCommandLists = nullptr;
static PresentFn s_OriginalPresent12 = nullptr;
static ID3D12CommandQueue* s_CommandQueue = nullptr;
static IDXGISwapChain3* s_SwapChain12 = nullptr;
static ID3D12Device* s_Device12 = nullptr;
static PresentCallback s_RenderCallback;
static bool s_D3D12Initialized = false;
static HWND s_GameWindow = nullptr;

// Forward declarations
void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* pQueue,
    UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists
);

HRESULT STDMETHODCALLTYPE HookedPresent12(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags
);

bool InitD3D12Hooks(HWND gameWindow) {
    s_GameWindow = gameWindow;
    
    // Check if D3D12 is loaded
    HMODULE d3d12Module = GetModuleHandleA("d3d12.dll");
    if (!d3d12Module) {
        Utils::Logger::Error("D3D12 not loaded");
        return false;
    }
    
    Utils::Logger::Info("D3D12 detected! Setting up hooks...");
    
    // Initialize MinHook first
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Utils::Logger::Error("MinHook init failed: %d", status);
        return false;
    }
    Utils::Logger::Info("MinHook initialized");
    
    // Create dummy D3D12 device to get vtable
    Utils::Logger::Info("Creating D3D12 device...");
    ComPtr<ID3D12Device> dummyDevice;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice));
    if (FAILED(hr)) {
        Utils::Logger::Error("D3D12CreateDevice failed: 0x%08X", hr);
        return false;
    }
    Utils::Logger::Info("D3D12 device created");
    
    // Create command queue to get vtable
    Utils::Logger::Info("Creating command queue...");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    
    ComPtr<ID3D12CommandQueue> dummyQueue;
    hr = dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue));
    if (FAILED(hr)) {
        Utils::Logger::Error("CreateCommandQueue failed: 0x%08X", hr);
        return false;
    }
    Utils::Logger::Info("Command queue created");
    
    // Get ExecuteCommandLists from vtable (index 10)
    void** queueVtable = *reinterpret_cast<void***>(dummyQueue.Get());
    void* executeCommandListsAddr = queueVtable[10];
    Utils::Logger::Info("ExecuteCommandLists addr: %p", executeCommandListsAddr);
    
    // Hook ExecuteCommandLists
    status = MH_CreateHook(
        executeCommandListsAddr,
        reinterpret_cast<void*>(&HookedExecuteCommandLists),
        reinterpret_cast<void**>(&s_OriginalExecuteCommandLists)
    );
    
    if (status != MH_OK) {
        Utils::Logger::Error("Hook ExecuteCommandLists failed: %d", status);
        return false;
    }
    Utils::Logger::Info("ExecuteCommandLists hooked");
    
    // Create dummy swap chain for Present hook
    Utils::Logger::Info("Creating swap chain...");
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = 100;
    swapChainDesc.Height = 100;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Utils::Logger::Error("CreateDXGIFactory1 failed: 0x%08X", hr);
        // Continue without Present hook - ExecuteCommandLists might be enough
    } else {
        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory->CreateSwapChainForHwnd(
            dummyQueue.Get(),
            gameWindow,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1
        );
        
        if (SUCCEEDED(hr)) {
            void** swapChainVtable = *reinterpret_cast<void***>(swapChain1.Get());
            void* presentAddr = swapChainVtable[8];
            
            status = MH_CreateHook(
                presentAddr,
                reinterpret_cast<void*>(&HookedPresent12),
                reinterpret_cast<void**>(&s_OriginalPresent12)
            );
            
            if (status == MH_OK) {
                Utils::Logger::Info("D3D12 Present hooked");
            } else {
                Utils::Logger::Warn("Present hook failed: %d (non-fatal)", status);
            }
        } else {
            Utils::Logger::Warn("CreateSwapChainForHwnd failed: 0x%08X (non-fatal)", hr);
        }
    }
    
    // Enable all hooks
    Utils::Logger::Info("Enabling hooks...");
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        Utils::Logger::Error("EnableHook failed: %d", status);
        return false;
    }
    
    Utils::Logger::Info("D3D12 hooks ready!");
    s_D3D12Initialized = true;
    
    return true;
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* pQueue,
    UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists
) {
    static bool firstCall = true;
    if (firstCall) {
        firstCall = false;
        s_CommandQueue = pQueue;
        pQueue->GetDevice(IID_PPV_ARGS(&s_Device12));
        Utils::Logger::Info("D3D12: Captured CommandQueue and Device!");
    }
    
    // Call original
    s_OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

HRESULT STDMETHODCALLTYPE HookedPresent12(
    IDXGISwapChain* pSwapChain,
    UINT SyncInterval,
    UINT Flags
) {
    static bool firstCall = true;
    if (firstCall) {
        firstCall = false;
        pSwapChain->QueryInterface(IID_PPV_ARGS(&s_SwapChain12));
        Utils::Logger::Info("D3D12: Captured SwapChain!");
    }
    
    // Call render callback if set
    if (s_RenderCallback) {
        s_RenderCallback(pSwapChain);
    }
    
    return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
}

void SetD3D12RenderCallback(PresentCallback callback) {
    s_RenderCallback = callback;
}

bool IsD3D12Available() {
    return GetModuleHandleA("d3d12.dll") != nullptr;
}

bool IsD3D12Initialized() {
    return s_D3D12Initialized;
}

} // namespace Core
} // namespace FiveMFrameGen

// Global wrapper for easy access from main.cpp
bool InitD3D12Hooks(HWND gameWindow) {
    return FiveMFrameGen::Core::InitD3D12Hooks(gameWindow);
}
