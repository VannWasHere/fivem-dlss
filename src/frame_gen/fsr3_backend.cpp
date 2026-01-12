/**
 * FSR 3 Frame Generation Backend Implementation
 */

#include "fsr3_backend.h"
#include "../utils/logger.h"

#include <algorithm>
#include <numeric>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace FiveMFrameGen {
namespace FrameGen {

// ============================================================================
// Shader Source (HLSL)
// ============================================================================

// Full-screen triangle vertex shader
static const char* g_FullscreenVS = R"(
struct VSOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput output;
    
    // Generate fullscreen triangle
    output.texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    
    return output;
}
)";

// Interpolation pixel shader
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

// Motion-compensated interpolation
float4 main(PSInput input) : SV_Target {
    // Sample motion at this location
    float2 motion = motionVectors.Sample(linearSampler, input.texcoord);
    
    // Calculate sample positions for both frames
    float2 prevUV = input.texcoord - motion * (1.0 - interpolationFactor);
    float2 currUV = input.texcoord + motion * interpolationFactor;
    
    // Sample both frames
    float4 prevColor = framePrev.Sample(linearSampler, prevUV);
    float4 currColor = frameCurr.Sample(linearSampler, currUV);
    
    // Blend based on interpolation factor
    float4 color = lerp(prevColor, currColor, interpolationFactor);
    
    // Optional sharpening pass
    if (sharpness > 0.0) {
        float4 blur = float4(0, 0, 0, 0);
        blur += frameCurr.Sample(linearSampler, input.texcoord + float2(-texelSize.x, 0));
        blur += frameCurr.Sample(linearSampler, input.texcoord + float2( texelSize.x, 0));
        blur += frameCurr.Sample(linearSampler, input.texcoord + float2(0, -texelSize.y));
        blur += frameCurr.Sample(linearSampler, input.texcoord + float2(0,  texelSize.y));
        blur *= 0.25;
        
        color = color + (color - blur) * sharpness;
    }
    
    return saturate(color);
}
)";

// Simple present shader (for texture copy)
static const char* g_PresentPS = R"(
Texture2D<float4> sourceTexture : register(t0);
SamplerState pointSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
    return sourceTexture.Sample(pointSampler, input.texcoord);
}
)";

// ============================================================================
// Implementation
// ============================================================================

FSR3FrameGenerator::FSR3FrameGenerator()
    : m_LastFrameTime(Clock::now())
{
}

FSR3FrameGenerator::~FSR3FrameGenerator() {
    Shutdown();
}

bool FSR3FrameGenerator::Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    IDXGISwapChain* swapChain
) {
    if (m_Initialized) {
        Utils::Logger::Warn("FSR3 backend already initialized");
        return true;
    }
    
    m_Device = device;
    m_Context = context;
    m_SwapChain = swapChain;
    
    // Get swap chain description
    DXGI_SWAP_CHAIN_DESC swapDesc;
    swapChain->GetDesc(&swapDesc);
    m_Width = swapDesc.BufferDesc.Width;
    m_Height = swapDesc.BufferDesc.Height;
    
    Utils::Logger::Info("Initializing FSR3 backend (%dx%d)", m_Width, m_Height);
    
    // Initialize frame buffer
    m_FrameBuffer = std::make_unique<FrameBuffer>();
    if (!m_FrameBuffer->Initialize(device, m_Width, m_Height, swapDesc.BufferDesc.Format)) {
        Utils::Logger::Error("Failed to initialize frame buffer");
        return false;
    }
    
    // Initialize motion vector calculator
    m_MotionCalc = std::make_unique<MotionVectorCalculator>();
    if (!m_MotionCalc->Initialize(device, m_Width, m_Height)) {
        Utils::Logger::Error("Failed to initialize motion calculator");
        return false;
    }
    
    // Create interpolated frame texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_Width;
    texDesc.Height = m_Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = swapDesc.BufferDesc.Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &m_InterpolatedFrame);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create interpolated frame: 0x%08X", hr);
        return false;
    }
    
    hr = device->CreateRenderTargetView(m_InterpolatedFrame, nullptr, &m_InterpolatedRTV);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create interpolated RTV: 0x%08X", hr);
        return false;
    }
    
    hr = device->CreateShaderResourceView(m_InterpolatedFrame, nullptr, &m_InterpolatedSRV);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create interpolated SRV: 0x%08X", hr);
        return false;
    }
    
    // Compile shaders
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    
    // Compile vertex shader
    hr = D3DCompile(g_FullscreenVS, strlen(g_FullscreenVS), "FullscreenVS",
        nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            Utils::Logger::Error("VS compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }
    
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, &m_FullscreenVS);
    vsBlob->Release();
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create vertex shader: 0x%08X", hr);
        return false;
    }
    
    // Compile interpolation pixel shader
    hr = D3DCompile(g_InterpolationPS, strlen(g_InterpolationPS), "InterpolationPS",
        nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            Utils::Logger::Error("Interpolation PS compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }
    
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &m_InterpolationPS);
    psBlob->Release();
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create interpolation shader: 0x%08X", hr);
        return false;
    }
    
    // Compile present pixel shader
    hr = D3DCompile(g_PresentPS, strlen(g_PresentPS), "PresentPS",
        nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            Utils::Logger::Error("Present PS compile error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        return false;
    }
    
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, &m_PresentPS);
    psBlob->Release();
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create present shader: 0x%08X", hr);
        return false;
    }
    
    // Create sampler
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    hr = device->CreateSamplerState(&samplerDesc, &m_LinearSampler);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create sampler: 0x%08X", hr);
        return false;
    }
    
    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;  // 4 floats
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    hr = device->CreateBuffer(&cbDesc, nullptr, &m_ConstantBuffer);
    if (FAILED(hr)) {
        Utils::Logger::Error("Failed to create constant buffer: 0x%08X", hr);
        return false;
    }
    
    m_Initialized = true;
    Utils::Logger::Info("FSR3 backend initialized successfully");
    
    return true;
}

void FSR3FrameGenerator::Shutdown() {
    if (!m_Initialized) return;
    
    Utils::Logger::Info("Shutting down FSR3 backend...");
    
    // Release resources
    if (m_ConstantBuffer) { m_ConstantBuffer->Release(); m_ConstantBuffer = nullptr; }
    if (m_LinearSampler) { m_LinearSampler->Release(); m_LinearSampler = nullptr; }
    if (m_PresentPS) { m_PresentPS->Release(); m_PresentPS = nullptr; }
    if (m_InterpolationPS) { m_InterpolationPS->Release(); m_InterpolationPS = nullptr; }
    if (m_FullscreenVS) { m_FullscreenVS->Release(); m_FullscreenVS = nullptr; }
    if (m_InterpolatedSRV) { m_InterpolatedSRV->Release(); m_InterpolatedSRV = nullptr; }
    if (m_InterpolatedRTV) { m_InterpolatedRTV->Release(); m_InterpolatedRTV = nullptr; }
    if (m_InterpolatedFrame) { m_InterpolatedFrame->Release(); m_InterpolatedFrame = nullptr; }
    
    m_MotionCalc.reset();
    m_FrameBuffer.reset();
    
    m_Initialized = false;
}

void FSR3FrameGenerator::ProcessFrame() {
    if (!m_Initialized) return;
    
    // Update timing
    auto now = Clock::now();
    float deltaMs = std::chrono::duration<float, std::milli>(now - m_LastFrameTime).count();
    m_LastFrameTime = now;
    
    // Capture current back buffer
    if (!CaptureBackBuffer()) {
        return;
    }
    
    // Need at least 2 frames for interpolation
    if (m_FrameBuffer->GetFrameCount() < 2) {
        m_FirstFrame = false;
        return;
    }
    
    // Check if we should generate a frame
    if (ShouldGenerateFrame()) {
        if (GenerateInterpolatedFrame()) {
            PresentGeneratedFrame();
            m_FramesGenerated++;
        }
    }
    
    // Update stats
    m_FrameTimeHistory.push_back(deltaMs);
    if (m_FrameTimeHistory.size() > FRAME_HISTORY_SIZE) {
        m_FrameTimeHistory.pop_front();
    }
    
    UpdateStats();
    m_TotalFrames++;
}

bool FSR3FrameGenerator::CaptureBackBuffer() {
    // Get back buffer
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
        return false;
    }
    
    // Push to frame buffer
    m_FrameBuffer->PushFrame(m_Context, backBuffer);
    backBuffer->Release();
    
    return true;
}

bool FSR3FrameGenerator::GenerateInterpolatedFrame() {
    // Get previous and current frames
    auto* prevSRV = m_FrameBuffer->GetFrameSRV(1);
    auto* currSRV = m_FrameBuffer->GetFrameSRV(0);
    
    if (!prevSRV || !currSRV) return false;
    
    // Calculate motion vectors
    m_MotionCalc->Calculate(m_Context, prevSRV, currSRV);
    auto* motionSRV = m_MotionCalc->GetMotionVectorsSRV();
    
    if (!motionSRV) return false;
    
    // Interpolate with factor 0.5 (middle frame)
    return Interpolate(prevSRV, currSRV, motionSRV, m_InterpolatedRTV, 0.5f);
}

bool FSR3FrameGenerator::Interpolate(
    ID3D11ShaderResourceView* framePrev,
    ID3D11ShaderResourceView* frameCurrent,
    ID3D11ShaderResourceView* motionVectors,
    ID3D11RenderTargetView* output,
    float interpolationFactor
) {
    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_Context->Map(m_ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        struct Constants {
            float interpolationFactor;
            float sharpness;
            float texelSizeX;
            float texelSizeY;
        };
        
        Constants* constants = static_cast<Constants*>(mapped.pData);
        constants->interpolationFactor = interpolationFactor;
        constants->sharpness = m_Sharpness;
        constants->texelSizeX = 1.0f / m_Width;
        constants->texelSizeY = 1.0f / m_Height;
        
        m_Context->Unmap(m_ConstantBuffer, 0);
    }
    
    // Set render state
    m_Context->OMSetRenderTargets(1, &output, nullptr);
    
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_Width);
    vp.Height = static_cast<float>(m_Height);
    vp.MaxDepth = 1.0f;
    m_Context->RSSetViewports(1, &vp);
    
    // Set shaders
    m_Context->VSSetShader(m_FullscreenVS, nullptr, 0);
    m_Context->PSSetShader(m_InterpolationPS, nullptr, 0);
    
    // Set resources
    ID3D11ShaderResourceView* srvs[] = { framePrev, frameCurrent, motionVectors };
    m_Context->PSSetShaderResources(0, 3, srvs);
    m_Context->PSSetSamplers(0, 1, &m_LinearSampler);
    m_Context->PSSetConstantBuffers(0, 1, &m_ConstantBuffer);
    
    // Draw fullscreen triangle
    m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_Context->IASetInputLayout(nullptr);
    m_Context->Draw(3, 0);
    
    // Cleanup
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    m_Context->PSSetShaderResources(0, 3, nullSRVs);
    
    return true;
}

void FSR3FrameGenerator::PresentGeneratedFrame() {
    // Present the interpolated frame to the swap chain
    // This is done before the actual Present call
    
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return;
    
    // Copy interpolated frame to back buffer
    m_Context->CopyResource(backBuffer, m_InterpolatedFrame);
    
    backBuffer->Release();
    
    // Present the interpolated frame
    m_SwapChain->Present(0, 0);
}

bool FSR3FrameGenerator::ShouldGenerateFrame() const {
    // Simple heuristic: generate frame every other real frame
    // In production, this would be more sophisticated based on timing
    return (m_TotalFrames % 2) == 1;
}

void FSR3FrameGenerator::UpdateStats() {
    if (m_FrameTimeHistory.empty()) return;
    
    // Calculate average frame time
    float sum = std::accumulate(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end(), 0.0f);
    m_FrameTimeMs = sum / m_FrameTimeHistory.size();
    
    // Calculate FPS
    m_BaseFPS = 1000.0f / m_FrameTimeMs;
    m_OutputFPS = m_BaseFPS * 2.0f;  // 2x with frame gen
}

void FSR3FrameGenerator::SetQuality(QualityPreset preset) {
    m_Quality = preset;
    
    // Adjust sharpness based on quality
    switch (preset) {
        case QualityPreset::Performance:
            m_Sharpness = 0.3f;
            break;
        case QualityPreset::Balanced:
            m_Sharpness = 0.5f;
            break;
        case QualityPreset::Quality:
            m_Sharpness = 0.7f;
            break;
    }
}

void FSR3FrameGenerator::SetSharpness(float sharpness) {
    m_Sharpness = std::clamp(sharpness, 0.0f, 1.0f);
}

bool FSR3FrameGenerator::IsSupported() const {
    // FSR3 frame generation works on all modern GPUs
    return true;
}

void FSR3FrameGenerator::Reset() {
    m_FirstFrame = true;
    m_FrameTimeHistory.clear();
    
    if (m_FrameBuffer) {
        m_FrameBuffer->Shutdown();
        
        DXGI_SWAP_CHAIN_DESC desc;
        m_SwapChain->GetDesc(&desc);
        m_FrameBuffer->Initialize(m_Device, desc.BufferDesc.Width, desc.BufferDesc.Height, 
            desc.BufferDesc.Format);
    }
}

} // namespace FrameGen
} // namespace FiveMFrameGen
