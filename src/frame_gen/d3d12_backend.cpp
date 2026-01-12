#include "d3d12_backend.h"
#include "../utils/logger.h"
#include <d3dcompiler.h>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace FiveMFrameGen {
namespace FrameGen {

// ============================================================================
// Shaders (HLSL)
// ============================================================================

static const char* g_RootSig = 
"RootFlags( ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ), "
"DescriptorTable( SRV(t0, numDescriptors=3), UAV(u0, numDescriptors=1), CBV(b0, numDescriptors=1) ), "
"StaticSampler( s0, filter = FILTER_MIN_MAG_MIP_LINEAR )";

static const char* g_VS = R"(
struct VSOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
)";

static const char* g_InterpolationPS = R"(
Texture2D<float4> framePrev : register(t0);
Texture2D<float4> frameCurr : register(t1);
Texture2D<float2> motionVectors : register(t2);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float interpolationFactor;
    float sharpness;
    float2 texelSize;
};

struct PSInput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 motion = motionVectors.Sample(linearSampler, input.texcoord);
    float2 prevUV = input.texcoord - motion * 0.5;
    float2 currUV = input.texcoord + motion * 0.5;
    
    float4 prevColor = framePrev.Sample(linearSampler, prevUV);
    float4 currColor = frameCurr.Sample(linearSampler, currUV);
    
    return lerp(prevColor, currColor, 0.5);
}
)";

// Simple pass-through compute for optical flow placeholder
static const char* g_OpticalFlowCS = R"(
RWTexture2D<float2> motionVectors : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    // Placeholder: zero motion
    motionVectors[DTid.xy] = float2(0, 0);
}
)";

// ============================================================================
// Implementation
// ============================================================================

D3D12FrameGenerator::D3D12FrameGenerator() {
}

D3D12FrameGenerator::~D3D12FrameGenerator() {
    Shutdown();
}

bool D3D12FrameGenerator::Initialize(ID3D12CommandQueue* commandQueue, IDXGISwapChain3* swapChain) {
    if (m_Initialized) return true;
    
    m_CommandQueue = commandQueue;
    m_SwapChain = swapChain;
    
    HRESULT hr = commandQueue->GetDevice(IID_PPV_ARGS(&m_Device));
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to get D3D12 device");
        return false;
    }
    
    // Get swapchain desc
    DXGI_SWAP_CHAIN_DESC swDesc;
    swapChain->GetDesc(&swDesc);
    m_Width = swDesc.BufferDesc.Width;
    m_Height = swDesc.BufferDesc.Height;
    
    if (!CreateDeviceResources(m_Device.Get())) return false;
    if (!CreateWindowSizeDependentResources(m_Width, m_Height)) return false;
    
    // Create fence for synchronization
    hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr)) return false;
    m_FenceValue = 1;
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    
    m_Initialized = true;
    Utils::Logger::Info("D3D12 Frame Generator Initialized (%dx%d)", m_Width, m_Height);
    
    return true;
}

void D3D12FrameGenerator::Shutdown() {
    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }
    
    // Release ComPtrs automatically
    m_Initialized = false;
}

bool D3D12FrameGenerator::CreateDeviceResources(ID3D12Device* device) {
    // Create descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 16; 
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvUavHeap)))) return false;
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 4;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap)))) return false;
    
    m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    
    // Create command allocator & list
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocator)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_CommandList)))) return false;
    m_CommandList->Close(); // Start closed
    
    return CompileShaders();
}

bool D3D12FrameGenerator::CompileShaders() {
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    
    // For simplicity, we just serialize a basic root signature here or dynamic compilation
    // But since we are restricted in tools, let's try to compile the root signature from string if possible, or build it manually
    // Manually building root signature is safer without complex d3dcompiler
    
    D3D12_DESCRIPTOR_RANGE ranges[3];
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    D3D12_ROOT_PARAMETER parameters[1];
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 3;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    
    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = parameters;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    
    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &error))) return false;
    if (FAILED(m_Device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)))) return false;
    
    // Compile HLSL
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_InterpolationPS, strlen(g_InterpolationPS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    
    if (!vsBlob || !psBlob) {
        Utils::Logger::Error("Failed to compile shaders");
        return false;
    }
    
    // Create PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_RootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_InterpolationPSO)))) return false;
    
    return true;
}

bool D3D12FrameGenerator::CreateWindowSizeDependentResources(UINT width, UINT height) {
    // Recreate textures
    m_FrameHistory[0].Reset();
    m_FrameHistory[1].Reset();
    m_InterpolatedFrame.Reset();
    m_MotionVectors.Reset();
    
    CreateTextureResource(m_Device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_FrameHistory[0], L"History0");
        
    CreateTextureResource(m_Device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_FrameHistory[1], L"History1");
        
    CreateTextureResource(m_Device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_InterpolatedFrame, L"Interpolated");
    
    CreateTextureResource(m_Device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, 
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_MotionVectors, L"MotionVec");
    
    // Create Constant Buffer (Upload heap for dynamic update)
    D3D12_HEAP_PROPERTIES uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256); // 256 byte aligned
    m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, 
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ConstantBuffer));
        
    return true;
}

void D3D12FrameGenerator::CreateTextureResource(
    ID3D12Device* device, 
    UINT width, UINT height, 
    DXGI_FORMAT format, 
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS flags,
    ComPtr<ID3D12Resource>& resource,
    LPCWSTR name
) {
    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, flags);
    
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (resource) resource->SetName(name);
}


void D3D12FrameGenerator::ProcessFrame() {
    if (!m_Initialized) return;
    
    // Synchronize frame execution
    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), m_InterpolationPSO.Get());
    
    // Get current backbuffer
    UINT backBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    m_SwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    
    // 1. Copy Backbuffer to History[m_CurrentFrameIndex]
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), 
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_CommandList->ResourceBarrier(1, &barrier);
    
    // Copy
    D3D12_RESOURCE_BARRIER copyDestBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_FrameHistory[m_CurrentFrameIndex].Get(), 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    m_CommandList->ResourceBarrier(1, &copyDestBarrier);
    
    m_CommandList->CopyResource(m_FrameHistory[m_CurrentFrameIndex].Get(), backBuffer.Get());
    
    D3D12_RESOURCE_BARRIER copyReadBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_FrameHistory[m_CurrentFrameIndex].Get(), 
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_CommandList->ResourceBarrier(1, &copyReadBarrier);
    
    // Restore backbuffer to PRE_PRESENT state for later
    D3D12_RESOURCE_BARRIER restoreBarrier = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), 
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    m_CommandList->ResourceBarrier(1, &restoreBarrier);
    
    m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % 2;
    m_TotalFrames++;
    
    // Only generate every other frame 
    if (m_TotalFrames % 2 == 0) {
        // Run Interpolation !
        // Setup Descriptors
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        
        // 1. Prev Frame SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        
        m_Device->CreateShaderResourceView(m_FrameHistory[(m_CurrentFrameIndex + 1) % 2].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_SrvDescriptorSize;
        
        // 2. Curr Frame SRV (the one we just copied)
        m_Device->CreateShaderResourceView(m_FrameHistory[m_CurrentFrameIndex].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_SrvDescriptorSize;
        
        // 3. Motion Vectors (Placeholder)
        D3D12_SHADER_RESOURCE_VIEW_DESC mvDesc = {};
        mvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        mvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        mvDesc.Texture2D.MipLevels = 1;
        mvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        m_Device->CreateShaderResourceView(m_MotionVectors.Get(), &mvDesc, srvHandle);
        
        // Setup RTV for Interpolated Frame
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_Device->CreateRenderTargetView(m_InterpolatedFrame.Get(), &rtvDesc, rtvHandle);
        
        // Render
        D3D12_RESOURCE_BARRIER renderBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_InterpolatedFrame.Get(), 
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_CommandList->ResourceBarrier(1, &renderBarrier);
        
        m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());
        m_CommandList->SetPipelineState(m_InterpolationPSO.Get());
        
        ID3D12DescriptorHeap* heaps[] = { m_SrvUavHeap.Get() };
        m_CommandList->SetDescriptorHeaps(1, heaps);
        m_CommandList->SetGraphicsRootDescriptorTable(0, m_SrvUavHeap->GetGPUDescriptorHandleForHeapStart());
        
        m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_Width, (float)m_Height, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)m_Width, (LONG)m_Height };
        m_CommandList->RSSetViewports(1, &viewport);
        m_CommandList->RSSetScissorRects(1, &scissor);
        
        m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_CommandList->DrawInstanced(3, 1, 0, 0);
        
        // Transition back to copy source
        D3D12_RESOURCE_BARRIER afterRenderBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_InterpolatedFrame.Get(), 
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_CommandList->ResourceBarrier(1, &afterRenderBarrier);
        
        // Present this frame! 
        // We copy interpolated content to Backbuffer
        // Wait.. if we copy to backbuffer, we overwrite the current game frame.
        // True Frame Gen inserts a Present call with the interpolated frame.
        // Since we are inside a Hooked Present, we can:
        // 1. Present the Interpolated Frame (by copying to SwapChain and calling OriginalPresent)
        // 2. Then proceed to Present the Real Frame.
        
        // For simplicity in this step: We just overwrite the current frame contents with interpolated one to prove it works.
        // To really boost FPS we need to call Swapchain->Present() TWICE.
        // But doing that inside a Hooked Present is dangerous (infinite recursion).
        // Solution: Call the TRAMPOLINE Present (s_OriginalPresent12).
        
        // Copy Interpolated -> Backbuffer
        D3D12_RESOURCE_BARRIER bbToDest = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), 
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        m_CommandList->ResourceBarrier(1, &bbToDest);
        
        m_CommandList->CopyResource(backBuffer.Get(), m_InterpolatedFrame.Get());
        
        D3D12_RESOURCE_BARRIER bbToPresent = CD3DX12_RESOURCE_BARRIER::Transition(backBuffer.Get(), 
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        m_CommandList->ResourceBarrier(1, &bbToPresent);
    }
    
    m_CommandList->Close();
    
    ID3D12CommandList* lists[] = { m_CommandList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, lists);
    
    // Wait
    m_CommandQueue->Signal(m_Fence.Get(), m_FenceValue);
    m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
    WaitForSingleObject(m_FenceEvent, INFINITE);
    m_FenceValue++;
}

void D3D12FrameGenerator::SetQuality(QualityPreset preset) { m_Quality = preset; }
void D3D12FrameGenerator::SetSharpness(float sharpness) { m_Sharpness = sharpness; }

}} // namespace
