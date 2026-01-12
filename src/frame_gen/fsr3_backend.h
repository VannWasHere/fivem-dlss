#pragma once

/**
 * FSR 3 Frame Generation Backend
 * 
 * Implements frame generation using AMD FidelityFX FSR 3
 */

#ifndef FIVEM_FRAMEGEN_FSR3_BACKEND_H
#define FIVEM_FRAMEGEN_FSR3_BACKEND_H

#include "frame_generator.h"
#include <chrono>
#include <deque>

namespace FiveMFrameGen {
namespace FrameGen {

/**
 * FSR 3 Frame Generation Implementation
 * 
 * Uses AMD's FSR 3 frame generation technology for AI-based frame interpolation.
 * Works on all modern GPUs (NVIDIA, AMD, Intel).
 */
class FSR3FrameGenerator : public IFrameGenerator {
public:
    FSR3FrameGenerator();
    ~FSR3FrameGenerator() override;
    
    // IFrameGenerator interface
    bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        IDXGISwapChain* swapChain
    ) override;
    
    void Shutdown() override;
    void ProcessFrame() override;
    void SetQuality(QualityPreset preset) override;
    void SetSharpness(float sharpness) override;
    
    float GetBaseFPS() const override { return m_BaseFPS; }
    float GetOutputFPS() const override { return m_OutputFPS; }
    float GetFrameTimeMs() const override { return m_FrameTimeMs; }
    uint64_t GetFramesGenerated() const override { return m_FramesGenerated; }
    
    Backend GetBackend() const override { return Backend::FSR3; }
    bool IsSupported() const override;
    void Reset() override;

private:
    /**
     * Capture current back buffer
     */
    bool CaptureBackBuffer();
    
    /**
     * Generate interpolated frame
     */
    bool GenerateInterpolatedFrame();
    
    /**
     * Present the generated frame
     */
    void PresentGeneratedFrame();
    
    /**
     * Update performance stats
     */
    void UpdateStats();
    
    /**
     * Check if we should generate a frame this cycle
     */
    bool ShouldGenerateFrame() const;
    
    /**
     * Interpolate between two frames
     */
    bool Interpolate(
        ID3D11ShaderResourceView* framePrev,
        ID3D11ShaderResourceView* frameCurrent,
        ID3D11ShaderResourceView* motionVectors,
        ID3D11RenderTargetView* output,
        float interpolationFactor
    );

private:
    // D3D11 resources
    ID3D11Device* m_Device = nullptr;
    ID3D11DeviceContext* m_Context = nullptr;
    IDXGISwapChain* m_SwapChain = nullptr;
    
    // Frame buffers
    std::unique_ptr<FrameBuffer> m_FrameBuffer;
    std::unique_ptr<MotionVectorCalculator> m_MotionCalc;
    
    // Interpolation resources
    ID3D11Texture2D* m_InterpolatedFrame = nullptr;
    ID3D11RenderTargetView* m_InterpolatedRTV = nullptr;
    ID3D11ShaderResourceView* m_InterpolatedSRV = nullptr;
    
    // Shaders
    ID3D11VertexShader* m_FullscreenVS = nullptr;
    ID3D11PixelShader* m_InterpolationPS = nullptr;
    ID3D11PixelShader* m_PresentPS = nullptr;
    ID3D11SamplerState* m_LinearSampler = nullptr;
    ID3D11Buffer* m_ConstantBuffer = nullptr;
    
    // Settings
    QualityPreset m_Quality = QualityPreset::Balanced;
    float m_Sharpness = 0.5f;
    
    // State
    bool m_Initialized = false;
    bool m_FirstFrame = true;
    UINT m_Width = 0;
    UINT m_Height = 0;
    
    // Stats
    float m_BaseFPS = 0.0f;
    float m_OutputFPS = 0.0f;
    float m_FrameTimeMs = 0.0f;
    uint64_t m_FramesGenerated = 0;
    uint64_t m_TotalFrames = 0;
    
    // Timing
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    
    TimePoint m_LastFrameTime;
    std::deque<float> m_FrameTimeHistory;
    static constexpr size_t FRAME_HISTORY_SIZE = 60;
    
    // Shader bytecode (embedded)
    static const unsigned char s_FullscreenVS[];
    static const size_t s_FullscreenVSSize;
    static const unsigned char s_InterpolationPS[];
    static const size_t s_InterpolationPSSize;
};

} // namespace FrameGen
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_FSR3_BACKEND_H
