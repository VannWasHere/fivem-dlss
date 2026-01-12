#pragma once

#include "../include/fivem_framegen.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <chrono>

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace FrameGen {

class D3D12FrameGenerator {
public:
    D3D12FrameGenerator();
    ~D3D12FrameGenerator();

    bool Initialize(ID3D12CommandQueue* commandQueue, IDXGISwapChain3* swapChain);
    void Shutdown();
    void ProcessFrame();
    
    // Configuration
    void SetQuality(QualityPreset preset);
    void SetSharpness(float sharpness);

private:
    bool CreateDeviceResources(ID3D12Device* device);
    bool CreateWindowSizeDependentResources(UINT width, UINT height);
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CompileShaders();
    bool CreateComputePipeline(); // For optical flow
    
    void RecordCommandList();
    bool ExecuteCommandList();
    
    // Helpers
    void CreateTextureResource(
        ID3D12Device* device, 
        UINT width, UINT height, 
        DXGI_FORMAT format, 
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS flags,
        ComPtr<ID3D12Resource>& resource,
        LPCWSTR name
    );

    // Core D3D12 objects
    ComPtr<ID3D12Device> m_Device;
    ComPtr<ID3D12CommandQueue> m_CommandQueue;
    ComPtr<IDXGISwapChain3> m_SwapChain;
    ComPtr<ID3D12CommandAllocator> m_CommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_CommandList;
    ComPtr<ID3D12Fence> m_Fence;
    UINT64 m_FenceValue = 0;
    HANDLE m_FenceEvent = nullptr;
    
    // Pipeline objects
    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_InterpolationPSO;
    ComPtr<ID3D12PipelineState> m_OpticalFlowPSO;
    ComPtr<ID3D12PipelineState> m_CopyPSO;
    
    // Descriptors
    ComPtr<ID3D12DescriptorHeap> m_SrvUavHeap;
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    UINT m_SrvDescriptorSize = 0;
    UINT m_RtvDescriptorSize = 0;
    
    // Resources
    static const int FrameHistoryCount = 2;
    ComPtr<ID3D12Resource> m_FrameHistory[FrameHistoryCount];
    ComPtr<ID3D12Resource> m_MotionVectors;
    ComPtr<ID3D12Resource> m_InterpolatedFrame;
    ComPtr<ID3D12Resource> m_ConstantBuffer;
    void* m_ConstantBufferData = nullptr;
    
    // State
    bool m_Initialized = false;
    UINT m_Width = 0;
    UINT m_Height = 0;
    QualityPreset m_Quality = QualityPreset::Balanced;
    float m_Sharpness = 0.5f;
    size_t m_CurrentFrameIndex = 0;
    size_t m_TotalFrames = 0;
    
    // Timing
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point m_LastFrameTime;
};

} // namespace FrameGen
} // namespace FiveMFrameGen
