/**
 * Frame Generator Implementation
 */

#include "frame_generator.h"
#include "fsr3_backend.h"
#include "../utils/logger.h"

#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace FiveMFrameGen {
namespace FrameGen {

// ============================================================================
// FrameBuffer Implementation
// ============================================================================

FrameBuffer::FrameBuffer() {
    for (size_t i = 0; i < MAX_FRAMES; ++i) {
        m_Frames[i] = nullptr;
        m_FrameSRVs[i] = nullptr;
    }
}

FrameBuffer::~FrameBuffer() {
    Shutdown();
}

bool FrameBuffer::Initialize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (!device) return false;
    
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    
    // Create frame textures
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    
    for (size_t i = 0; i < MAX_FRAMES; ++i) {
        HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_Frames[i]);
        if (FAILED(hr)) {
            Utils::Logger::Error("Failed to create frame buffer %zu: 0x%08X", i, hr);
            Shutdown();
            return false;
        }
        
        hr = device->CreateShaderResourceView(m_Frames[i], &srvDesc, &m_FrameSRVs[i]);
        if (FAILED(hr)) {
            Utils::Logger::Error("Failed to create frame SRV %zu: 0x%08X", i, hr);
            Shutdown();
            return false;
        }
    }
    
    Utils::Logger::Info("Frame buffer initialized (%dx%d, %d frames)", width, height, MAX_FRAMES);
    return true;
}

void FrameBuffer::Shutdown() {
    for (size_t i = 0; i < MAX_FRAMES; ++i) {
        if (m_FrameSRVs[i]) {
            m_FrameSRVs[i]->Release();
            m_FrameSRVs[i] = nullptr;
        }
        if (m_Frames[i]) {
            m_Frames[i]->Release();
            m_Frames[i] = nullptr;
        }
    }
    
    m_CurrentIndex = 0;
    m_FrameCount = 0;
}

void FrameBuffer::PushFrame(ID3D11DeviceContext* context, ID3D11Texture2D* frame) {
    if (!context || !frame) return;
    
    // Copy to next slot
    m_CurrentIndex = (m_CurrentIndex + 1) % MAX_FRAMES;
    context->CopyResource(m_Frames[m_CurrentIndex], frame);
    
    if (m_FrameCount < MAX_FRAMES) {
        m_FrameCount++;
    }
}

ID3D11Texture2D* FrameBuffer::GetFrame(size_t index) const {
    if (index >= m_FrameCount) return nullptr;
    
    size_t actualIndex = (m_CurrentIndex + MAX_FRAMES - index) % MAX_FRAMES;
    return m_Frames[actualIndex];
}

ID3D11ShaderResourceView* FrameBuffer::GetFrameSRV(size_t index) const {
    if (index >= m_FrameCount) return nullptr;
    
    size_t actualIndex = (m_CurrentIndex + MAX_FRAMES - index) % MAX_FRAMES;
    return m_FrameSRVs[actualIndex];
}

// ============================================================================
// MotionVectorCalculator Implementation
// ============================================================================

// Optical flow compute shader (HLSL)
static const char* g_OpticalFlowShader = R"(
// Simple block-matching optical flow
// This is a basic implementation - production would use more sophisticated algorithms

Texture2D<float4> prevFrame : register(t0);
Texture2D<float4> currFrame : register(t1);
RWTexture2D<float2> motionVectors : register(u0);

SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0) {
    uint2 resolution;
    uint blockSize;
    uint searchRadius;
};

// Convert to grayscale for matching
float Luminance(float4 color) {
    return dot(color.rgb, float3(0.299, 0.587, 0.114));
}

// Calculate sum of absolute differences
float SAD(int2 pos, int2 offset) {
    float sum = 0.0;
    
    [unroll]
    for (int y = 0; y < 8; y++) {
        [unroll]
        for (int x = 0; x < 8; x++) {
            int2 prevPos = pos + int2(x, y);
            int2 currPos = prevPos + offset;
            
            if (currPos.x >= 0 && currPos.x < (int)resolution.x &&
                currPos.y >= 0 && currPos.y < (int)resolution.y) {
                float prevLum = Luminance(prevFrame[prevPos]);
                float currLum = Luminance(currFrame[currPos]);
                sum += abs(prevLum - currLum);
            }
        }
    }
    
    return sum;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    int2 blockPos = int2(DTid.xy) * 8;
    
    if (blockPos.x >= (int)resolution.x || blockPos.y >= (int)resolution.y) {
        return;
    }
    
    // Search for best match
    float bestSAD = 1e10;
    int2 bestOffset = int2(0, 0);
    
    int sr = (int)searchRadius;
    
    for (int dy = -sr; dy <= sr; dy++) {
        for (int dx = -sr; dx <= sr; dx++) {
            float sad = SAD(blockPos, int2(dx, dy));
            
            if (sad < bestSAD) {
                bestSAD = sad;
                bestOffset = int2(dx, dy);
            }
        }
    }
    
    // Store motion vector (normalized to -1 to 1 range)
    float2 mv = float2(bestOffset) / float2(resolution);
    motionVectors[DTid.xy] = mv;
}
)";

MotionVectorCalculator::MotionVectorCalculator() = default;

MotionVectorCalculator::~MotionVectorCalculator() {
    Shutdown();
}

bool MotionVectorCalculator::Initialize(ID3D11Device* device, UINT width, UINT height) {
    if (!device) return false;
    
    m_Device = device;
    m_Width = width;
    m_Height = height;
    
    // Create motion vector texture (R16G16 for motion x,y)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width / 8;  // One vector per 8x8 block
    texDesc.Height = height / 8;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_MotionVectors);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create motion vector texture: 0x%08X", hr);
        return false;
    }
    
    // Create SRV
    hr = device->CreateShaderResourceView(m_MotionVectors, nullptr, &m_MotionVectorsSRV);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create motion vector SRV: 0x%08X", hr);
        return false;
    }
    
    // Create UAV
    hr = device->CreateUnorderedAccessView(m_MotionVectors, nullptr, &m_MotionVectorsUAV);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create motion vector UAV: 0x%08X", hr);
        return false;
    }
    
    // Create compute shader
    if (!CreateShader()) {
        Utils::Logger::Error("Failed to create optical flow shader");
        return false;
    }
    
    Utils::Logger::Info("Motion vector calculator initialized");
    return true;
}

void MotionVectorCalculator::Shutdown() {
    if (m_MotionVectorsUAV) {
        m_MotionVectorsUAV->Release();
        m_MotionVectorsUAV = nullptr;
    }
    if (m_MotionVectorsSRV) {
        m_MotionVectorsSRV->Release();
        m_MotionVectorsSRV = nullptr;
    }
    if (m_MotionVectors) {
        m_MotionVectors->Release();
        m_MotionVectors = nullptr;
    }
    if (m_OpticalFlowCS) {
        m_OpticalFlowCS->Release();
        m_OpticalFlowCS = nullptr;
    }
}

bool MotionVectorCalculator::CreateShader() {
    // Note: In production, this shader should be pre-compiled
    // For now, we compile at runtime using D3DCompile
    
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    
    // Link d3dcompiler.lib or load D3DCompile dynamically
    typedef HRESULT(WINAPI* pD3DCompile)(
        LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**
    );
    
    HMODULE hD3DCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hD3DCompiler) {
        Utils::Logger::Error("Failed to load d3dcompiler_47.dll");
        return false;
    }
    
    pD3DCompile D3DCompileFunc = (pD3DCompile)GetProcAddress(hD3DCompiler, "D3DCompile");
    if (!D3DCompileFunc) {
        FreeLibrary(hD3DCompiler);
        Utils::Logger::Error("Failed to get D3DCompile function");
        return false;
    }
    
    HRESULT hr = D3DCompileFunc(
        g_OpticalFlowShader,
        strlen(g_OpticalFlowShader),
        "OpticalFlow.hlsl",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        0,
        0,
        &shaderBlob,
        &errorBlob
    );
    
    FreeLibrary(hD3DCompiler);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            Utils::Logger::Error("Shader compile error: %s", 
                (const char*)errorBlob->GetBufferPointer());
        }
        return false;
    }
    
    hr = m_Device->CreateComputeShader(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        nullptr,
        &m_OpticalFlowCS
    );
    
    return SUCCEEDED(hr);
}

ID3D11Texture2D* MotionVectorCalculator::Calculate(
    ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* framePrev,
    ID3D11ShaderResourceView* frameCurrent
) {
    if (!context || !framePrev || !frameCurrent || !m_OpticalFlowCS) {
        return nullptr;
    }
    
    // Bind shader
    context->CSSetShader(m_OpticalFlowCS, nullptr, 0);
    
    // Bind input textures
    ID3D11ShaderResourceView* srvs[] = { framePrev, frameCurrent };
    context->CSSetShaderResources(0, 2, srvs);
    
    // Bind output
    context->CSSetUnorderedAccessViews(0, 1, &m_MotionVectorsUAV, nullptr);
    
    // Dispatch
    context->Dispatch(
        (m_Width / 8 + 7) / 8,
        (m_Height / 8 + 7) / 8,
        1
    );
    
    // Unbind
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context->CSSetShaderResources(0, 2, nullSRVs);
    
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    
    return m_MotionVectors;
}

// ============================================================================
// Factory Function
// ============================================================================

std::unique_ptr<IFrameGenerator> CreateFrameGenerator(Backend backend) {
    switch (backend) {
        case Backend::FSR3:
            return std::make_unique<FSR3FrameGenerator>();
            
        case Backend::DLSS3:
            // DLSS3 requires RTX 40 series and additional SDK
            // For now, fall back to FSR3
            Utils::Logger::Warn("DLSS3 backend not yet implemented, using FSR3");
            return std::make_unique<FSR3FrameGenerator>();
            
        case Backend::OpticalFlow:
            // Generic optical flow backend
            return std::make_unique<FSR3FrameGenerator>();
            
        case Backend::None:
        default:
            return nullptr;
    }
}

} // namespace FrameGen
} // namespace FiveMFrameGen
