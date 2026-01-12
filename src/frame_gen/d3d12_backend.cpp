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

static const char* g_OpticalFlowCS = R"(
RWTexture2D<float2> motionVectors : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
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
    
    DXGI_SWAP_CHAIN_DESC swDesc;
    swapChain->GetDesc(&swDesc);
    m_Width = swDesc.BufferDesc.Width;
    m_Height = swDesc.BufferDesc.Height;
    
    if (!CreateDeviceResources(m_Device.Get())) return false;
    if (!CreateWindowSizeDependentResources(m_Width, m_Height)) return false;
    
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
    m_Initialized = false;
}

bool D3D12FrameGenerator::CreateDeviceResources(ID3D12Device* device) {
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
    
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocator)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_CommandList)))) return false;
    m_CommandList->Close(); 
    
    return CompileShaders();
}

bool D3D12FrameGenerator::CompileShaders() {
    ComPtr<ID3DBlob> error;
    
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[2].NumDescriptors = 1;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    D3D12_ROOT_PARAMETER parameters[1] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 3;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
    
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_InterpolationPS, strlen(g_InterpolationPS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    
    if (!vsBlob || !psBlob) {
        Utils::Logger::Error("Failed to compile shaders");
        return false;
    }
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_RootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    
    D3D12_RASTERIZER_DESC rasterizerState = {};
    rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerState.FrontCounterClockwise = FALSE;
    rasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerState.DepthClipEnable = TRUE;
    rasterizerState.MultisampleEnable = FALSE;
    rasterizerState.AntialiasedLineEnable = FALSE;
    rasterizerState.ForcedSampleCount = 0;
    rasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.RasterizerState = rasterizerState;

    D3D12_BLEND_DESC blendState = {};
    blendState.AlphaToCoverageEnable = FALSE;
    blendState.IndependentBlendEnable = FALSE;
    blendState.RenderTarget[0].BlendEnable = FALSE;
    blendState.RenderTarget[0].LogicOpEnable = FALSE;
    blendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendState;
    
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_InterpolationPSO)))) return false;
    
    return true;
}

bool D3D12FrameGenerator::CreateWindowSizeDependentResources(UINT width, UINT height) {
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
    
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;
    
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Alignment = 0;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.SampleDesc.Quality = 0;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    
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
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (resource) resource->SetName(name);
}


void D3D12FrameGenerator::ProcessFrame() {
    if (!m_Initialized) return;
    
    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), m_InterpolationPSO.Get());
    
    UINT backBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    m_SwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_CommandList->ResourceBarrier(1, &barrier);
    
    D3D12_RESOURCE_BARRIER copyDestBarrier = {};
    copyDestBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyDestBarrier.Transition.pResource = m_FrameHistory[m_CurrentFrameIndex].Get();
    copyDestBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    copyDestBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_CommandList->ResourceBarrier(1, &copyDestBarrier);
    
    m_CommandList->CopyResource(m_FrameHistory[m_CurrentFrameIndex].Get(), backBuffer.Get());
    
    D3D12_RESOURCE_BARRIER copyReadBarrier = {};
    copyReadBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyReadBarrier.Transition.pResource = m_FrameHistory[m_CurrentFrameIndex].Get();
    copyReadBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    copyReadBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_CommandList->ResourceBarrier(1, &copyReadBarrier);
    
    D3D12_RESOURCE_BARRIER restoreBarrier = {};
    restoreBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBarrier.Transition.pResource = backBuffer.Get();
    restoreBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restoreBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_CommandList->ResourceBarrier(1, &restoreBarrier);
    
    m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % 2;
    m_TotalFrames++;
    
    if (m_TotalFrames % 2 == 0) {
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        
        m_Device->CreateShaderResourceView(m_FrameHistory[(m_CurrentFrameIndex + 1) % 2].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_SrvDescriptorSize;
        
        m_Device->CreateShaderResourceView(m_FrameHistory[m_CurrentFrameIndex].Get(), &srvDesc, srvHandle);
        srvHandle.ptr += m_SrvDescriptorSize;
        
        D3D12_SHADER_RESOURCE_VIEW_DESC mvDesc = {};
        mvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        mvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        mvDesc.Texture2D.MipLevels = 1;
        mvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        m_Device->CreateShaderResourceView(m_MotionVectors.Get(), &mvDesc, srvHandle);
        
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_Device->CreateRenderTargetView(m_InterpolatedFrame.Get(), &rtvDesc, rtvHandle);
        
        D3D12_RESOURCE_BARRIER renderBarrier = {};
        renderBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        renderBarrier.Transition.pResource = m_InterpolatedFrame.Get();
        renderBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        renderBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
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
        
        D3D12_RESOURCE_BARRIER afterRenderBarrier = {};
        afterRenderBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        afterRenderBarrier.Transition.pResource = m_InterpolatedFrame.Get();
        afterRenderBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        afterRenderBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_CommandList->ResourceBarrier(1, &afterRenderBarrier);
        
        D3D12_RESOURCE_BARRIER bbToDest = {};
        bbToDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bbToDest.Transition.pResource = backBuffer.Get();
        bbToDest.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        bbToDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        m_CommandList->ResourceBarrier(1, &bbToDest);
        
        m_CommandList->CopyResource(backBuffer.Get(), m_InterpolatedFrame.Get());
        
        D3D12_RESOURCE_BARRIER bbToPresent = {};
        bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bbToPresent.Transition.pResource = backBuffer.Get();
        bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_CommandList->ResourceBarrier(1, &bbToPresent);
    }
    
    m_CommandList->Close();
    
    ID3D12CommandList* lists[] = { m_CommandList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, lists);
    
    m_CommandQueue->Signal(m_Fence.Get(), m_FenceValue);
    m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
    WaitForSingleObject(m_FenceEvent, INFINITE);
    m_FenceValue++;
}

void D3D12FrameGenerator::SetQuality(QualityPreset preset) { m_Quality = preset; }
void D3D12FrameGenerator::SetSharpness(float sharpness) { m_Sharpness = sharpness; }

}} // namespace
