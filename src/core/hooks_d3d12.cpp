/**
 * DirectX 12 Hooking Implementation for FiveM
 * 
 * This file handles capturing the real FiveM D3D12 resources and rendering the UI.
 */

#include "hooks.h"
#include "../utils/logger.h"
#include "../upscaler/upscaler_d3d12.h"
#include "../overlay/imgui_overlay.h"
#include "../utils/config.h" 

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory> 
#include <vector>

using Microsoft::WRL::ComPtr;

// Config Externs
namespace FiveMFrameGen { 
    extern Config g_FrameGenConfig; 
    extern Stats g_Stats; 
}

// Map to main.cpp LogRaw
extern void LogRaw(const char* fmt, ...);

namespace FiveMFrameGen {
namespace Core {

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
using RSSetScissorRectsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);

// Static members
static ExecuteCommandListsFn s_OriginalExecuteCommandLists = nullptr;
static PresentFn s_OriginalPresent12 = nullptr;
static RSSetViewportsFn s_OriginalRSSetViewports = nullptr;
static RSSetScissorRectsFn s_OriginalRSSetScissorRects = nullptr;

static ID3D12CommandQueue* s_CommandQueue = nullptr;
static ID3D12Device* s_Device12 = nullptr;
static bool s_D3D12Initialized = false;

// Upscaler
static std::unique_ptr<FiveMFrameGen::Upscaler::D3D12Upscaler> g_D3D12Upscaler;
static UINT s_DisplayWidth = 0;
static UINT s_DisplayHeight = 0;

// UI Resources
static FiveMFrameGen::Overlay::ImGuiOverlay* s_Overlay = nullptr;
static ComPtr<ID3D12DescriptorHeap> s_RtvHeap;
static ComPtr<ID3D12CommandAllocator> s_UIAllocator;
static ComPtr<ID3D12GraphicsCommandList> s_UICommandList;
static UINT s_RtvDescriptorSize = 0;

// Forward declarations
void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
HRESULT STDMETHODCALLTYPE HookedPresent12(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
void STDMETHODCALLTYPE HookedRSSetViewports(ID3D12GraphicsCommandList* pList, UINT NumViewports, const D3D12_VIEWPORT* pViewports);
void STDMETHODCALLTYPE HookedRSSetScissorRects(ID3D12GraphicsCommandList* pList, UINT NumRects, const D3D12_RECT* pRects);

void SetD3D12Overlay(void* overlay) {
    s_Overlay = (FiveMFrameGen::Overlay::ImGuiOverlay*)overlay;
    LogRaw("D3D12: Overlay instance linked to hooks: 0x%p", s_Overlay);
}

void EnsureUIResources(ID3D12Device* device) {
    if (s_RtvHeap) return;
    
    LogRaw("D3D12: Creating UI Resources...");
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 8; 
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&s_RtvHeap)))) {
        LogRaw("D3D12 ERROR: Failed to create UI RTV heap");
        return;
    }
    
    s_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_UIAllocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_UIAllocator.Get(), nullptr, IID_PPV_ARGS(&s_UICommandList));
    s_UICommandList->Close();
    LogRaw("D3D12: UI Resources Ready");
}

bool InitD3D12Hooks(HWND gameWindow) {
    LogRaw("D3D12: InitD3D12Hooks starting...");
    HMODULE d3d12Module = GetModuleHandleA("d3d12.dll");
    if (!d3d12Module) return false;
    
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    
    ComPtr<ID3D12Device> dummyDevice;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice)))) return false;
    
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> dummyQueue;
    if (FAILED(dummyDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&dummyQueue)))) return false;
    
    ComPtr<ID3D12CommandAllocator> dummyAllocator;
    dummyDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dummyAllocator));
    ComPtr<ID3D12GraphicsCommandList> dummyList;
    dummyDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dummyAllocator.Get(), nullptr, IID_PPV_ARGS(&dummyList));
    
    // 1. Hook ExecuteCommandLists
    void** queueVtable = *reinterpret_cast<void***>(dummyQueue.Get());
    static bool hookedECL = false;
    if (!hookedECL) {
        MH_STATUS status = MH_CreateHook(queueVtable[10], (void*)&HookedExecuteCommandLists, (void**)&s_OriginalExecuteCommandLists);
        LogRaw("D3D12: Hook ExecuteCommandLists status: %d", status);
        hookedECL = true;
    }
    
    // 2. Hook Present (obtain vtable from a dummy swapchain)
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 100; sd.Height = 100;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    HWND dummyHwnd = CreateWindowA("STATIC", "Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ComPtr<IDXGIFactory2> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGISwapChain1> swapChain;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(dummyQueue.Get(), dummyHwnd, &sd, nullptr, nullptr, &swapChain))) {
            void** scVtable = *reinterpret_cast<void***>(swapChain.Get());
            MH_STATUS status = MH_CreateHook(scVtable[8], (void*)&HookedPresent12, (void**)&s_OriginalPresent12);
            LogRaw("D3D12: Hook Present status: %d", status);
        }
    }
    if (dummyHwnd) DestroyWindow(dummyHwnd);
    
    if (dummyList) {
        void** listVtable = *reinterpret_cast<void***>(dummyList.Get());
        MH_CreateHook(listVtable[44], (void*)&HookedRSSetViewports, (void**)&s_OriginalRSSetViewports);
        MH_CreateHook(listVtable[45], (void*)&HookedRSSetScissorRects, (void**)&s_OriginalRSSetScissorRects);
    }
    
    MH_EnableHook(MH_ALL_HOOKS);
    LogRaw("D3D12: Hooks enabled system-wide");
    
    g_D3D12Upscaler = std::make_unique<FiveMFrameGen::Upscaler::D3D12Upscaler>();
    s_D3D12Initialized = true;
    return true;
}

static UINT s_ViewportSetCount = 0;

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    if (!s_CommandQueue) {
        D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            s_CommandQueue = pQueue;
            pQueue->GetDevice(IID_PPV_ARGS(&s_Device12));
            LogRaw("D3D12: CAPTURED Queue (0x%p) from ExecuteCommandLists", s_CommandQueue);
        }
    }
    s_OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

void STDMETHODCALLTYPE HookedRSSetViewports(ID3D12GraphicsCommandList* pList, UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
    if (g_D3D12Upscaler && s_DisplayWidth > 0 && NumViewports > 0 && pViewports) {
        float scale = g_D3D12Upscaler->GetScaleFactor();
        if (scale < 0.99f) {
            bool isFullScreenViewport = (pViewports[0].Width == (float)s_DisplayWidth && pViewports[0].Height == (float)s_DisplayHeight);
            if (isFullScreenViewport) {
                s_ViewportSetCount++;
                if (s_ViewportSetCount <= 2) { 
                    std::vector<D3D12_VIEWPORT> newViewports(NumViewports);
                    for (UINT i=0; i<NumViewports; i++) {
                        newViewports[i] = pViewports[i];
                        newViewports[i].Width *= scale;
                        newViewports[i].Height *= scale;
                    }
                    s_OriginalRSSetViewports(pList, NumViewports, newViewports.data());
                    return;
                }
            }
        }
    }
    s_OriginalRSSetViewports(pList, NumViewports, pViewports);
}

void STDMETHODCALLTYPE HookedRSSetScissorRects(ID3D12GraphicsCommandList* pList, UINT NumRects, const D3D12_RECT* pRects) {
    if (g_D3D12Upscaler && s_DisplayWidth > 0 && NumRects > 0 && pRects) {
        float scale = g_D3D12Upscaler->GetScaleFactor();
        if (scale < 0.99f) {
            if ((pRects[0].right - pRects[0].left) == (LONG)s_DisplayWidth) {
                if (s_ViewportSetCount <= 2) {
                     std::vector<D3D12_RECT> newRects(NumRects);
                    for (UINT i=0; i<NumRects; i++) {
                        newRects[i] = pRects[i];
                        newRects[i].right = newRects[i].left + (LONG)((pRects[i].right - pRects[i].left) * scale);
                        newRects[i].bottom = newRects[i].top + (LONG)((pRects[i].bottom - pRects[i].top) * scale);
                    }
                    s_OriginalRSSetScissorRects(pList, NumRects, newRects.data());
                    return;
                }
            }
        }
    }
    s_OriginalRSSetScissorRects(pList, NumRects, pRects);
}

HRESULT STDMETHODCALLTYPE HookedPresent12(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    s_ViewportSetCount = 0; 
    
    // CRITICAL: Robust Resource Capture
    if (!s_CommandQueue || !s_Device12) {
        ID3D12CommandQueue* queue = nullptr;
        // In D3D12, GetDevice on a SwapChain returns the CommandQueue passed at creation!
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&queue)))) {
            s_CommandQueue = queue;
            if (SUCCEEDED(s_CommandQueue->GetDevice(IID_PPV_ARGS(&s_Device12)))) {
                LogRaw("D3D12: SUCCESS! Resources captured in Present. Queue: 0x%p, Device: 0x%p", s_CommandQueue, s_Device12);
            } else {
                LogRaw("D3D12: Got object 0x%p from SwapChain but it's not a Queue (failed GetDevice)", queue);
                queue->Release();
                s_CommandQueue = nullptr;
                // Fallback: This object might be the Device itself in some wrappers
                pSwapChain->GetDevice(IID_PPV_ARGS(&s_Device12));
            }
        }
    }
    
    if (!s_CommandQueue || !s_Device12) {
        static int warnCounter = 0;
        if (warnCounter < 5) {
            LogRaw("D3D12 WARNING: Skipping frame, resources not yet captured. (Q:0x%p, D:0x%p)", s_CommandQueue, s_Device12);
            warnCounter++;
        }
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }

    DXGI_SWAP_CHAIN_DESC desc;
    pSwapChain->GetDesc(&desc);
    s_DisplayWidth = desc.BufferDesc.Width;
    s_DisplayHeight = desc.BufferDesc.Height;

    // 1. Process Upscaler
    static bool generatorInitialized = false;
    if (!generatorInitialized && g_D3D12Upscaler) {
        IDXGISwapChain3* swapChain3 = nullptr;
        if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
            if (g_D3D12Upscaler->Initialize(s_CommandQueue, swapChain3)) {
                generatorInitialized = true;
                LogRaw("D3D12: Upscaler Backend Ready");
            }
            swapChain3->Release();
        }
    }
    
    if (generatorInitialized && g_D3D12Upscaler) {
        g_D3D12Upscaler->ProcessFrame();
    }
    
    // 2. Render Overlay
    if (s_Overlay && s_Overlay->IsVisible()) {
        EnsureUIResources(s_Device12);
        
        if (s_UICommandList && s_UIAllocator) {
            if (s_Overlay->InitializeD3D12(s_Device12, desc.BufferCount, desc.BufferDesc.Format, s_CommandQueue, desc.OutputWindow)) {
                s_UIAllocator->Reset();
                s_UICommandList->Reset(s_UIAllocator.Get(), nullptr);
                
                ComPtr<ID3D12Resource> backBuffer;
                IDXGISwapChain3* swapChain3 = (IDXGISwapChain3*)pSwapChain; 
                UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
                pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
                
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = s_RtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtvHandle.ptr += (backBufferIndex * s_RtvDescriptorSize);
                s_Device12->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);
                
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = backBuffer.Get();
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                s_UICommandList->ResourceBarrier(1, &barrier);
                
                s_Overlay->RenderD3D12(FiveMFrameGen::g_FrameGenConfig, FiveMFrameGen::g_Stats, s_UICommandList.Get(), rtvHandle);
                
                D3D12_RESOURCE_BARRIER restore = {};
                restore.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                restore.Transition.pResource = backBuffer.Get();
                restore.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                restore.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                s_UICommandList->ResourceBarrier(1, &restore);
                
                s_UICommandList->Close();
                ID3D12CommandList* lists[] = { s_UICommandList.Get() };
                s_CommandQueue->ExecuteCommandLists(1, lists);
            }
        }
    }
    
    return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
}

bool IsD3D12Available() { return GetModuleHandleA("d3d12.dll") != nullptr; }
bool IsD3D12Initialized() { return s_D3D12Initialized; }

void SetD3D12Quality(int qualityIndex) {
    if (g_D3D12Upscaler) {
        using namespace FiveMFrameGen::Upscaler;
        QualityMode mode = QualityMode::Quality;
        switch(qualityIndex) {
            case 0: mode = QualityMode::Performance; break;
            case 1: mode = QualityMode::Balanced; break;
            case 2: mode = QualityMode::Quality; break;
            default: mode = QualityMode::Quality; break;
        }
        g_D3D12Upscaler->SetQuality(mode);
    }
}

} // namespace Core
} // namespace FiveMFrameGen

bool InitD3D12Hooks(HWND gameWindow) {
    return FiveMFrameGen::Core::InitD3D12Hooks(gameWindow);
}

void SetD3D12Quality(int quality) {
    FiveMFrameGen::Core::SetD3D12Quality(quality);
}

void SetD3D12Overlay(void* overlay) {
    FiveMFrameGen::Core::SetD3D12Overlay(overlay);
}
