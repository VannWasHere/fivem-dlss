#include "upscaler_d3d12.h"
#include "../utils/logger.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace FiveMFrameGen {
namespace Upscaler {

// Fallback Shaders
static const char* g_RootSig = 
"RootFlags( ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT ), "
"DescriptorTable( SRV(t0, numDescriptors=1), CBV(b0, numDescriptors=1) ), "
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

static const char* g_UpscalePS = R"(
Texture2D<float4> inputTexture : register(t0);
SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    float2 scaleFactor;
    float2 padding;
};

struct PSInput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    float2 sampleUV = input.texcoord;
    return inputTexture.Sample(linearSampler, sampleUV);
}
)";

D3D12Upscaler::D3D12Upscaler() : m_LastFrameTime(std::chrono::high_resolution_clock::now()) {}

D3D12Upscaler::~D3D12Upscaler() {
    Shutdown();
}

bool D3D12Upscaler::Initialize(ID3D12CommandQueue* commandQueue, IDXGISwapChain3* swapChain) {
    if (m_Initialized) return true;
    
    m_CommandQueue = commandQueue;
    m_SwapChain = swapChain;
    
    HRESULT hr = commandQueue->GetDevice(IID_PPV_ARGS(&m_Device));
    if (FAILED(hr)) return false;
    
    DXGI_SWAP_CHAIN_DESC swDesc;
    swapChain->GetDesc(&swDesc);
    m_DisplayWidth = swDesc.BufferDesc.Width;
    m_DisplayHeight = swDesc.BufferDesc.Height;
    
    if (!CreateDeviceResources(m_Device.Get())) return false;
    if (!CreateWindowSizeDependentResources(m_DisplayWidth, m_DisplayHeight)) return false;
    
    hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr)) return false;
    m_FenceValue = 1;
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    
    // Try Init FSR2
    if (!InitializeFSR2()) {
        Utils::Logger::Warn("FSR2 Init failed/missing, falling back to Bilinear");
    }
    
    m_Initialized = true;
    Utils::Logger::Info("D3D12 Upscaler Initialized (%dx%d)", m_DisplayWidth, m_DisplayHeight);
    return true;
}

void D3D12Upscaler::Shutdown() {
    DestroyFSR2();
    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }
    m_Initialized = false;
}

float D3D12Upscaler::GetScaleFactor() const {
    switch (m_Quality) {
        case QualityMode::Quality: return 0.666667f;
        case QualityMode::Balanced: return 0.588235f;
        case QualityMode::Performance: return 0.5f;
        default: return 1.0f;
    }
}

bool D3D12Upscaler::InitializeFSR2() {
    if (m_Fsr2Initialized) DestroyFSR2();

    float scale = GetScaleFactor();
    UINT renderWidth = (UINT)(m_DisplayWidth * scale);
    UINT renderHeight = (UINT)(m_DisplayHeight * scale);

    FfxFsr2ContextDescription contextDesc = {};
    contextDesc.flags = 0; 
    contextDesc.maxRenderSize[0] = (float)renderWidth;
    contextDesc.maxRenderSize[1] = (float)renderHeight;
    contextDesc.displaySize[0] = (float)m_DisplayWidth;
    contextDesc.displaySize[1] = (float)m_DisplayHeight;
    contextDesc.device = m_Device.Get();
    
    if (ffxFsr2ContextCreate(&m_Fsr2Context, &contextDesc) == FFX_OK) {
        m_Fsr2Initialized = true;
        Utils::Logger::Info("FSR2 Context Created");
        return true;
    }
    return false;
}

void D3D12Upscaler::DestroyFSR2() {
    if (m_Fsr2Initialized) {
        ffxFsr2ContextDestroy(&m_Fsr2Context);
        m_Fsr2Initialized = false;
    }
}

bool D3D12Upscaler::CreateDeviceResources(ID3D12Device* device) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocator)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_CommandList)))) return false;
    m_CommandList->Close(); 
    
    // Create Heaps for Fallback
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2; // Input, CBV
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_SrvUavHeap)))) return false;
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_RtvHeap)))) return false;
    
    m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    
    return CompileShaders();
}

bool D3D12Upscaler::CompileShaders() {
    ComPtr<ID3DBlob> error;
    
    // Define Root Signature for Fallback
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    
    D3D12_ROOT_PARAMETER parameters[1] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
    D3DCompile(g_UpscalePS, strlen(g_UpscalePS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_RootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    
    D3D12_RASTERIZER_DESC rasterizerState = {};
    rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState = rasterizerState;

    D3D12_BLEND_DESC blendState = {};
    blendState.RenderTarget[0].BlendEnable = FALSE;
    blendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendState;
    
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_UpscalePSO)))) return false;
    return true;
}

bool D3D12Upscaler::CreateWindowSizeDependentResources(UINT width, UINT height) {
    m_InputTexture.Reset();
    m_OutputTexture.Reset();
    m_DummyDepth.Reset();
    m_DummyMotionVectors.Reset();
    m_ConstantBuffer.Reset(); // Reset CB too
    
    // Calculate Render Size
    float scale = GetScaleFactor();
    UINT renderWidth = (UINT)(width * scale);
    UINT renderHeight = (UINT)(height * scale);

    // Input texture (Copy of backbuffer relevant region)
    CreateTextureResource(m_Device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, m_InputTexture, L"UpscalerInput");
        
    // Output texture (Result)
    CreateTextureResource(m_Device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_OutputTexture, L"UpscalerOutput");
    
    // Constant Buffer (for fallback)
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = 256;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    
    m_Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, 
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ConstantBuffer));

    // Dummy Resources for FSR2
    CreateTextureResource(m_Device.Get(), renderWidth, renderHeight, DXGI_FORMAT_D32_FLOAT, 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, m_DummyDepth, L"DummyDepth");
        
    CreateTextureResource(m_Device.Get(), renderWidth, renderHeight, DXGI_FORMAT_R16G16_FLOAT, 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_DummyMotionVectors, L"DummyVectors");

    // Re-init FSR2 if needed (screen resize)
    if (m_Fsr2Initialized) InitializeFSR2();
    
    return true;
}

void D3D12Upscaler::CreateTextureResource(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& resource, LPCWSTR name) {
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
    desc.Flags = flags;
    
    device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource));
    if (resource) resource->SetName(name);
}

void D3D12Upscaler::ProcessFrame() {
    if (!m_Initialized) return;
    
    auto now = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration<float, std::milli>(now - m_LastFrameTime).count();
    m_LastFrameTime = now;
    m_FrameIndex++;

    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), nullptr);
    
    // 1. Copy Relevant Region of BackBuffer -> InputTexture
    UINT backBufferIndex = m_SwapChain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    m_SwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_CommandList->ResourceBarrier(1, &barrier);
    
    D3D12_RESOURCE_BARRIER inputBarrier = {};
    inputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    inputBarrier.Transition.pResource = m_InputTexture.Get();
    inputBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; 
    inputBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_CommandList->ResourceBarrier(1, &inputBarrier);
    
    // Copy Subregion (Source is Valid Region of BackBuffer)
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_InputTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = backBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    
    D3D12_BOX srcBox = {};
    D3D12_RESOURCE_DESC inputDesc = m_InputTexture->GetDesc();
    srcBox.left = 0;
    srcBox.top = 0;
    srcBox.right = (UINT)inputDesc.Width;
    srcBox.bottom = (UINT)inputDesc.Height;
    srcBox.front = 0;
    srcBox.back = 1;
    
    m_CommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);
    
    // Transition Input to Shader Read
    D3D12_RESOURCE_BARRIER readyBarrier = {};
    readyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readyBarrier.Transition.pResource = m_InputTexture.Get();
    readyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    readyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_CommandList->ResourceBarrier(1, &readyBarrier);
    
    // 2. Dispatch (FSR2 or Fallback)
    bool fsr2Run = false;
    
    if (m_Fsr2Initialized) {
         D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        uavBarrier.Transition.pResource = m_OutputTexture.Get();
        uavBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
        uavBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        m_CommandList->ResourceBarrier(1, &uavBarrier);

        DispatchFSR2(m_CommandList.Get());
        fsr2Run = true;

        D3D12_RESOURCE_BARRIER uavToCopy = {};
        uavToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        uavToCopy.Transition.pResource = m_OutputTexture.Get();
        uavToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        uavToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_CommandList->ResourceBarrier(1, &uavToCopy);
    } else {
        // FALLBACK: Bilinear Blit
        // Transition Output to RenderTarget
        D3D12_RESOURCE_BARRIER rtBarrier = {};
        rtBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rtBarrier.Transition.pResource = m_OutputTexture.Get();
        rtBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
        rtBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_CommandList->ResourceBarrier(1, &rtBarrier);
        
        // Update Constants
        float scale = GetScaleFactor();
        struct { float s[2]; float p[2]; } cbData = { {scale, scale}, {0,0} };
        void* pData;
        m_ConstantBuffer->Map(0, nullptr, &pData);
        memcpy(pData, &cbData, sizeof(cbData));
        m_ConstantBuffer->Unmap(0, nullptr);
        
        // Setup Pipeline
        m_CommandList->SetGraphicsRootSignature(m_RootSignature.Get());
        m_CommandList->SetPipelineState(m_UpscalePSO.Get());
        
        ID3D12DescriptorHeap* heaps[] = { m_SrvUavHeap.Get() };
        m_CommandList->SetDescriptorHeaps(1, heaps);
        
        // Create Views in Heap
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_SrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        m_Device->CreateShaderResourceView(m_InputTexture.Get(), &srvDesc, srvHandle);
        
        D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = srvHandle;
        cbvHandle.ptr += m_SrvDescriptorSize; // Offset by 1 descriptor
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = m_ConstantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = 256;
        m_Device->CreateConstantBufferView(&cbvDesc, cbvHandle);
        
        m_CommandList->SetGraphicsRootDescriptorTable(0, m_SrvUavHeap->GetGPUDescriptorHandleForHeapStart());
        
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_Device->CreateRenderTargetView(m_OutputTexture.Get(), nullptr, rtvHandle);
        m_CommandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)m_DisplayWidth, (float)m_DisplayHeight, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)m_DisplayWidth, (LONG)m_DisplayHeight };
        m_CommandList->RSSetViewports(1, &viewport);
        m_CommandList->RSSetScissorRects(1, &scissor);
        
        m_CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_CommandList->DrawInstanced(3, 1, 0, 0);

        // Transition Output to Copy Source
        D3D12_RESOURCE_BARRIER rtToCopy = {};
        rtToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rtToCopy.Transition.pResource = m_OutputTexture.Get();
        rtToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rtToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        m_CommandList->ResourceBarrier(1, &rtToCopy);
    }
    
    // 3. Copy Output -> Full BackBuffer
    D3D12_RESOURCE_BARRIER bbToDest = {};
    bbToDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bbToDest.Transition.pResource = backBuffer.Get();
    bbToDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bbToDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_CommandList->ResourceBarrier(1, &bbToDest);
    
    m_CommandList->CopyResource(backBuffer.Get(), m_OutputTexture.Get());
    
    // Restore states
    D3D12_RESOURCE_BARRIER restoreBB = {};
    restoreBB.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreBB.Transition.pResource = backBuffer.Get();
    restoreBB.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    restoreBB.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    
    D3D12_RESOURCE_BARRIER restoreOutput = {};
    restoreOutput.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreOutput.Transition.pResource = m_OutputTexture.Get();
    restoreOutput.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    restoreOutput.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ; 
    
    D3D12_RESOURCE_BARRIER restoreInput = {};
    restoreInput.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restoreInput.Transition.pResource = m_InputTexture.Get();
    restoreInput.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    restoreInput.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

    D3D12_RESOURCE_BARRIER finals[] = { restoreBB, restoreOutput, restoreInput };
    m_CommandList->ResourceBarrier(3, finals);
    
    m_CommandList->Close();
    
    ID3D12CommandList* lists[] = { m_CommandList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, lists);
    
    m_CommandQueue->Signal(m_Fence.Get(), m_FenceValue);
    m_Fence->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
    WaitForSingleObject(m_FenceEvent, INFINITE);
    m_FenceValue++;
}

void D3D12Upscaler::DispatchFSR2(ID3D12GraphicsCommandList* commandList) {
    if (!m_Fsr2Initialized) return;

    FfxFsr2DispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = commandList;
    dispatchDesc.color = m_InputTexture.Get();
    dispatchDesc.depth = m_DummyDepth.Get(); // Ideally real depth
    dispatchDesc.motionVectors = m_DummyMotionVectors.Get(); // Ideally real vectors
    dispatchDesc.exposure = nullptr;
    dispatchDesc.reactive = nullptr;
    dispatchDesc.transparencyAndComposition = nullptr;
    dispatchDesc.output = m_OutputTexture.Get();
    
    dispatchDesc.jitterOffset[0] = 0.0f; // Needs jitter logic
    dispatchDesc.jitterOffset[1] = 0.0f;
    
    D3D12_RESOURCE_DESC inDesc = m_InputTexture->GetDesc();
    dispatchDesc.motionVectorScale[0] = (float)inDesc.Width;
    dispatchDesc.motionVectorScale[1] = (float)inDesc.Height;
    dispatchDesc.renderSize[0] = (float)inDesc.Width;
    dispatchDesc.renderSize[1] = (float)inDesc.Height;
    
    dispatchDesc.enableSharpening = true;
    dispatchDesc.sharpness = 0.5f;
    dispatchDesc.frameTimeDelta = 16.6f; // Calculate real delta
    dispatchDesc.preExposure = 1.0f;
    dispatchDesc.reset = (m_FrameIndex < 2);
    dispatchDesc.cameraNear = 0.1f;
    dispatchDesc.cameraFar = 1000.0f;
    dispatchDesc.cameraFovAngleVertical = 1.047f; // 60 deg
    
    ffxFsr2ContextDispatch(&m_Fsr2Context, &dispatchDesc);
}

void D3D12Upscaler::SetQuality(QualityMode quality) { 
    if (m_Quality != quality) {
        m_Quality = quality;
        CreateWindowSizeDependentResources(m_DisplayWidth, m_DisplayHeight);
    }
}

}} // namespace
