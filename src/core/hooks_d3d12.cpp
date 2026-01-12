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
#include <d3d11.h>
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

// D3D11 fallback - when swap chain is actually D3D11
static ID3D11Device* s_Device11 = nullptr;
static ID3D11DeviceContext* s_Context11 = nullptr;
static ID3D11RenderTargetView* s_RTV11 = nullptr;
static bool s_UsingD3D11Fallback = false;
static bool s_D3D11OverlayInitialized = false;
static IDXGISwapChain* s_LastSwapChain = nullptr; // Track swap chain changes

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

// Fence for UI command sync
static ComPtr<ID3D12Fence> s_UIFence;
static HANDLE s_UIFenceEvent = nullptr;
static UINT64 s_UIFenceValue = 0;
static bool s_OwnCommandQueue = false; // Track if we created our own queue

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
    
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_UIAllocator)))) {
        LogRaw("D3D12 ERROR: Failed to create UI command allocator");
        return;
    }
    
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_UIAllocator.Get(), nullptr, IID_PPV_ARGS(&s_UICommandList)))) {
        LogRaw("D3D12 ERROR: Failed to create UI command list");
        return;
    }
    s_UICommandList->Close();
    
    // Create fence for synchronization
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_UIFence)))) {
        LogRaw("D3D12 ERROR: Failed to create UI fence");
        return;
    }
    
    s_UIFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!s_UIFenceEvent) {
        LogRaw("D3D12 ERROR: Failed to create fence event");
        return;
    }
    
    s_UIFenceValue = 1;
    LogRaw("D3D12: UI Resources Ready (with fence sync)");
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
    static bool firstCall = true;
    if (firstCall) {
        LogRaw("D3D12: ExecuteCommandLists HOOKED! Queue: 0x%p, NumLists: %u", pQueue, NumCommandLists);
        firstCall = false;
    }
    
    // Only capture if we don't have a queue yet AND this is a direct queue
    if (!s_CommandQueue && pQueue) {
        D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            s_CommandQueue = pQueue;
            s_CommandQueue->AddRef(); // Keep it alive
            pQueue->GetDevice(IID_PPV_ARGS(&s_Device12));
            LogRaw("D3D12: CAPTURED Queue (0x%p) + Device (0x%p) from ExecuteCommandLists", s_CommandQueue, s_Device12);
        }
    }
    
    if (s_OriginalExecuteCommandLists) {
        s_OriginalExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
    }
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

// Global flag to completely disable overlay on crash
static bool s_OverlayDisabled = false;
static int s_FrameCount = 0;

HRESULT STDMETHODCALLTYPE HookedPresent12(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Always call original first if we're disabled or have issues
    if (!s_OriginalPresent12) {
        return E_FAIL;
    }
    
    if (!pSwapChain || s_OverlayDisabled) {
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }
    
    // Wait for game to stabilize - skip first 100 frames
    s_FrameCount++;
    if (s_FrameCount < 100) {
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }
    
    // Log only once after warmup
    static bool warmupLogged = false;
    if (!warmupLogged) {
        LogRaw("D3D12: Present hook active after warmup (frame %d)", s_FrameCount);
        warmupLogged = true;
    }
    
    // Detect swap chain change (happens when joining server, resizing, etc.)
    if (s_LastSwapChain != pSwapChain) {
        LogRaw("D3D12: SwapChain changed from 0x%p to 0x%p", s_LastSwapChain, pSwapChain);
        
        // Reset frame counter to allow new swap chain to stabilize
        s_FrameCount = 0;
        s_LastSwapChain = pSwapChain;
        
        // Just reset flags - don't release resources yet, let them be recreated lazily
        s_D3D11OverlayInitialized = false;
        s_UsingD3D11Fallback = false;
        
        // Release only the RTV as it's swap chain specific
        if (s_RTV11) { 
            s_RTV11->Release(); 
            s_RTV11 = nullptr; 
        }
        
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }
    
    s_ViewportSetCount = 0;
    
    // STEP 1: Validate we have a real IDXGISwapChain3 (required for D3D12)
    static bool diagLogged = false;
    IDXGISwapChain3* swapChain3 = nullptr;
    HRESULT hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    
    if (!diagLogged) {
        LogRaw("D3D12: HookedPresent12 FIRST CALL");
        LogRaw("D3D12:   SwapChain ptr: 0x%p", pSwapChain);
        LogRaw("D3D12:   QueryInterface(IDXGISwapChain3): 0x%08X", hr);
        
        if (FAILED(hr)) {
            // Check what interfaces ARE supported
            IDXGISwapChain1* sc1 = nullptr;
            IDXGISwapChain2* sc2 = nullptr;
            ID3D11Device* d3d11 = nullptr;
            
            pSwapChain->QueryInterface(IID_PPV_ARGS(&sc1));
            pSwapChain->QueryInterface(IID_PPV_ARGS(&sc2));
            pSwapChain->GetDevice(IID_PPV_ARGS(&d3d11));
            
            LogRaw("D3D12:   Has IDXGISwapChain1: %s", sc1 ? "YES" : "NO");
            LogRaw("D3D12:   Has IDXGISwapChain2: %s", sc2 ? "YES" : "NO");
            LogRaw("D3D12:   Has ID3D11Device: %s", d3d11 ? "YES" : "NO");
            
            if (sc1) sc1->Release();
            if (sc2) sc2->Release();
            if (d3d11) { 
                LogRaw("D3D12: THIS IS A D3D11 SWAPCHAIN! D3D12 overlay NOT possible here.");
                d3d11->Release();
            }
        }
        diagLogged = true;
    }
    
    // Check if we should use D3D11 fallback (swap chain is D3D11, not D3D12)
    if (FAILED(hr) || !swapChain3) {
        // Try D3D11 path (only if not disabled)
        if (!s_OverlayDisabled && !s_UsingD3D11Fallback && !s_Device11) {
            hr = pSwapChain->GetDevice(IID_PPV_ARGS(&s_Device11));
            if (SUCCEEDED(hr) && s_Device11) {
                s_Device11->GetImmediateContext(&s_Context11);
                if (s_Context11) {
                    s_UsingD3D11Fallback = true;
                    LogRaw("D3D11: SUCCESS! Using D3D11 fallback. Device: 0x%p, Context: 0x%p", s_Device11, s_Context11);
                } else {
                    LogRaw("D3D11 ERROR: GetImmediateContext failed");
                    s_Device11->Release();
                    s_Device11 = nullptr;
                }
            } else {
                LogRaw("D3D11: GetDevice failed HR=0x%08X", hr);
            }
        }
        // Continue to D3D11 rendering below
    } else {
        // STEP 2: Try to get D3D12 device
        if (!s_Device12 && !s_UsingD3D11Fallback) {
            hr = swapChain3->GetDevice(IID_PPV_ARGS(&s_Device12));
            if (SUCCEEDED(hr) && s_Device12) {
                LogRaw("D3D12: SUCCESS! Got FiveM's D3D12 device: 0x%p", s_Device12);
            } else {
                LogRaw("D3D12: GetDevice(ID3D12Device) failed (HR=0x%08X), trying D3D11...", hr);
                
                // Fall back to D3D11
                hr = swapChain3->GetDevice(IID_PPV_ARGS(&s_Device11));
                if (SUCCEEDED(hr) && s_Device11) {
                    s_Device11->GetImmediateContext(&s_Context11);
                    s_UsingD3D11Fallback = true;
                    LogRaw("D3D11: Fallback SUCCESS! Device: 0x%p, Context: 0x%p", s_Device11, s_Context11);
                }
            }
        }
        swapChain3->Release();
    }
    
    // === D3D11 RENDERING PATH ===
    if (s_UsingD3D11Fallback && s_Device11 && s_Context11 && s_Overlay) {
        // Only render when overlay is visible to avoid unnecessary work
        if (s_Overlay->IsVisible()) {
            // Initialize D3D11 overlay once (lazy init when first needed)
            if (!s_D3D11OverlayInitialized) {
                DXGI_SWAP_CHAIN_DESC desc = {};
                HRESULT hr = pSwapChain->GetDesc(&desc);
                if (SUCCEEDED(hr) && desc.OutputWindow && IsWindow(desc.OutputWindow)) {
                    LogRaw("D3D11: Attempting overlay init. Window: 0x%p", desc.OutputWindow);
                    if (s_Overlay->Initialize(s_Device11, s_Context11, desc.OutputWindow)) {
                        s_D3D11OverlayInitialized = true;
                        LogRaw("D3D11: Overlay ImGui initialized successfully!");
                    } else {
                        LogRaw("D3D11 ERROR: Overlay initialization failed - disabling");
                        s_OverlayDisabled = true;
                    }
                } else {
                    LogRaw("D3D11: Waiting for valid window (HR=0x%08X, HWND=0x%p)", hr, desc.OutputWindow);
                }
            }
            
            // Render D3D11 overlay (only if initialized)
            if (s_D3D11OverlayInitialized && !s_OverlayDisabled) {
                // Get back buffer and create RTV
                ID3D11Texture2D* backBuffer = nullptr;
                HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
                if (SUCCEEDED(hr) && backBuffer) {
                    // Release old RTV if exists
                    if (s_RTV11) {
                        s_RTV11->Release();
                        s_RTV11 = nullptr;
                    }
                    
                    hr = s_Device11->CreateRenderTargetView(backBuffer, nullptr, &s_RTV11);
                    if (SUCCEEDED(hr) && s_RTV11) {
                        s_Context11->OMSetRenderTargets(1, &s_RTV11, nullptr);
                        s_Overlay->SetRenderTarget(s_RTV11);
                        s_Overlay->Render(FiveMFrameGen::g_FrameGenConfig, FiveMFrameGen::g_Stats);
                    }
                    backBuffer->Release();
                }
            }
        }
        
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }
    
    // === D3D12 RENDERING PATH ===
    if (!s_Device12) {
        return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
    }
    
    // Create command queue on FiveM's device
    if (!s_CommandQueue) {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;
        
        hr = s_Device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&s_CommandQueue));
        if (SUCCEEDED(hr)) {
            s_OwnCommandQueue = true;
            LogRaw("D3D12: Created CommandQueue on FiveM's device: 0x%p", s_CommandQueue);
        } else {
            LogRaw("D3D12 ERROR: CreateCommandQueue failed, HR=0x%08X", hr);
            return s_OriginalPresent12(pSwapChain, SyncInterval, Flags);
        }
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
    
    // 2. Initialize & Render Overlay
    // Initialize once even if not visible, then only render when visible
    static bool overlayInitialized = false;
    if (s_Overlay && s_CommandQueue) {
        EnsureUIResources(s_Device12);
        
        if (s_UICommandList && s_UIAllocator) {
            // Initialize ImGui on first valid frame
            if (!overlayInitialized) {
                if (s_Overlay->InitializeD3D12(s_Device12, desc.BufferCount, desc.BufferDesc.Format, s_CommandQueue, desc.OutputWindow)) {
                    overlayInitialized = true;
                    LogRaw("D3D12: Overlay ImGui initialized successfully!");
                } else {
                    LogRaw("D3D12 ERROR: Overlay InitializeD3D12 failed");
                }
            }
            
            // Only render when visible
            if (overlayInitialized && s_Overlay->IsVisible()) {
                s_UIAllocator->Reset();
                s_UICommandList->Reset(s_UIAllocator.Get(), nullptr);
                
                ComPtr<ID3D12Resource> backBuffer;
                IDXGISwapChain3* swapChain3 = nullptr;
                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
                    UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
                    pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
                    
                    if (backBuffer) {
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = s_RtvHeap->GetCPUDescriptorHandleForHeapStart();
                        rtvHandle.ptr += (backBufferIndex * s_RtvDescriptorSize);
                        s_Device12->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);
                        
                        D3D12_RESOURCE_BARRIER barrier = {};
                        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        barrier.Transition.pResource = backBuffer.Get();
                        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        s_UICommandList->ResourceBarrier(1, &barrier);
                        
                        s_Overlay->RenderD3D12(FiveMFrameGen::g_FrameGenConfig, FiveMFrameGen::g_Stats, s_UICommandList.Get(), rtvHandle);
                        
                        D3D12_RESOURCE_BARRIER restore = {};
                        restore.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        restore.Transition.pResource = backBuffer.Get();
                        restore.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        restore.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                        restore.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        s_UICommandList->ResourceBarrier(1, &restore);
                        
                        s_UICommandList->Close();
                        ID3D12CommandList* lists[] = { s_UICommandList.Get() };
                        s_CommandQueue->ExecuteCommandLists(1, lists);
                        
                        // Signal and wait for fence (only if we own the queue)
                        if (s_OwnCommandQueue && s_UIFence && s_UIFenceEvent) {
                            const UINT64 fenceVal = s_UIFenceValue++;
                            s_CommandQueue->Signal(s_UIFence.Get(), fenceVal);
                            
                            if (s_UIFence->GetCompletedValue() < fenceVal) {
                                s_UIFence->SetEventOnCompletion(fenceVal, s_UIFenceEvent);
                                WaitForSingleObject(s_UIFenceEvent, 100); // Wait max 100ms
                            }
                        }
                    }
                    swapChain3->Release();
                }
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
