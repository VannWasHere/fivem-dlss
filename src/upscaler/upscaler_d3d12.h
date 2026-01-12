#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <chrono>

#include "fsr2/ffx_fsr2.h"

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace Upscaler {

enum class QualityMode {
    Quality,    // 1.5x scale (67%)
    Balanced,   // 1.7x scale (58%)
    Performance // 2.0x scale (50%)
};

class D3D12Upscaler {
public:
    D3D12Upscaler();
    ~D3D12Upscaler();

    bool Initialize(ID3D12CommandQueue* commandQueue, IDXGISwapChain3* swapChain);
    void Shutdown();
    void ProcessFrame(); 

    void SetQuality(QualityMode quality);
    QualityMode GetQuality() const { return m_Quality; }

    float GetScaleFactor() const;

private:
    bool CreateDeviceResources(ID3D12Device* device);
    bool CreateWindowSizeDependentResources(UINT width, UINT height);
    bool CompileShaders(); // Fallback shaders
    
    // FSR2 Methods
    bool InitializeFSR2();
    void DispatchFSR2(ID3D12GraphicsCommandList* commandList);
    void DestroyFSR2();

    void CreateTextureResource(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& resource, LPCWSTR name);

    // Resources
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain3> m_SwapChain;
    ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_CommandList;
    ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValue;
    HANDLE m_FenceEvent;

    // Fallback Pipeline
    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_UpscalePSO;
    ComPtr<ID3D12DescriptorHeap> m_SrvUavHeap;
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    UINT m_SrvDescriptorSize = 0;
    UINT m_RtvDescriptorSize = 0;

    // FSR2 Context
    FfxFsr2Context m_Fsr2Context = {};
    bool m_Fsr2Initialized = false;

    // Buffers
    ComPtr<ID3D12Resource> m_InputTexture; // Copy of BackBuffer (Low Res)
    ComPtr<ID3D12Resource> m_OutputTexture; // Upscaled Result
    ComPtr<ID3D12Resource> m_ConstantBuffer;
    
    // For FSR2 stability, we need a depth buffer if available, but for now we might skip it or clear it
    // FSR2 really needs Depth and Motion Vectors.
    // We will provide a dummy depth if we can't capture the game's one.
    ComPtr<ID3D12Resource> m_DummyDepth;
    ComPtr<ID3D12Resource> m_DummyMotionVectors;

    UINT m_DisplayWidth = 0;
    UINT m_DisplayHeight = 0;
    QualityMode m_Quality = QualityMode::Quality;
    bool m_Initialized = false;
    
    // Timing
    std::chrono::high_resolution_clock::time_point m_LastFrameTime;
    UINT m_FrameIndex = 0;
};

}} // namespace
