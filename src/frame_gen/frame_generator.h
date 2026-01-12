#pragma once

/**
 * Frame Generator Interface
 * 
 * Abstract interface for frame generation backends (FSR3, DLSS3, etc.)
 */

#ifndef FIVEM_FRAMEGEN_FRAME_GENERATOR_H
#define FIVEM_FRAMEGEN_FRAME_GENERATOR_H

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <memory>
#include <cstdint>

#include "../include/fivem_framegen.h"

namespace FiveMFrameGen {
namespace FrameGen {

/**
 * Abstract frame generator interface
 */
class IFrameGenerator {
public:
    virtual ~IFrameGenerator() = default;
    
    /**
     * Initialize the frame generator
     * 
     * @param device D3D11 device
     * @param context D3D11 device context
     * @param swapChain DXGI swap chain
     * @return True if initialization succeeded
     */
    virtual bool Initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        IDXGISwapChain* swapChain
    ) = 0;
    
    /**
     * Shutdown the frame generator
     */
    virtual void Shutdown() = 0;
    
    /**
     * Process the current frame and generate interpolated frame if needed
     */
    virtual void ProcessFrame() = 0;
    
    /**
     * Set quality preset
     */
    virtual void SetQuality(QualityPreset preset) = 0;
    
    /**
     * Set sharpness level (0-1)
     */
    virtual void SetSharpness(float sharpness) = 0;
    
    /**
     * Get the base (actual rendered) FPS
     */
    virtual float GetBaseFPS() const = 0;
    
    /**
     * Get the output FPS (with frame generation)
     */
    virtual float GetOutputFPS() const = 0;
    
    /**
     * Get the frame time in milliseconds
     */
    virtual float GetFrameTimeMs() const = 0;
    
    /**
     * Get total frames generated
     */
    virtual uint64_t GetFramesGenerated() const = 0;
    
    /**
     * Get the backend type
     */
    virtual Backend GetBackend() const = 0;
    
    /**
     * Check if the backend is available on current hardware
     */
    virtual bool IsSupported() const = 0;
    
    /**
     * Reset the frame generator state (e.g., after scene changes)
     */
    virtual void Reset() = 0;
};

/**
 * Frame buffer for storing frame history
 */
class FrameBuffer {
public:
    static constexpr size_t MAX_FRAMES = 4;
    
    FrameBuffer();
    ~FrameBuffer();
    
    bool Initialize(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format);
    void Shutdown();
    
    /**
     * Push a new frame to the buffer
     */
    void PushFrame(ID3D11DeviceContext* context, ID3D11Texture2D* frame);
    
    /**
     * Get frame by index (0 = current, 1 = previous, etc.)
     */
    ID3D11Texture2D* GetFrame(size_t index) const;
    
    /**
     * Get shader resource view for a frame
     */
    ID3D11ShaderResourceView* GetFrameSRV(size_t index) const;
    
    /**
     * Get the number of available frames
     */
    size_t GetFrameCount() const { return m_FrameCount; }
    
    /**
     * Get buffer dimensions
     */
    UINT GetWidth() const { return m_Width; }
    UINT GetHeight() const { return m_Height; }

private:
    ID3D11Texture2D* m_Frames[MAX_FRAMES] = {};
    ID3D11ShaderResourceView* m_FrameSRVs[MAX_FRAMES] = {};
    
    size_t m_CurrentIndex = 0;
    size_t m_FrameCount = 0;
    
    UINT m_Width = 0;
    UINT m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
    
    ID3D11Device* m_Device = nullptr;
};

/**
 * Motion vector calculator using optical flow
 */
class MotionVectorCalculator {
public:
    MotionVectorCalculator();
    ~MotionVectorCalculator();
    
    bool Initialize(ID3D11Device* device, UINT width, UINT height);
    void Shutdown();
    
    /**
     * Calculate motion vectors between two frames
     * 
     * @param context Device context
     * @param framePrev Previous frame
     * @param frameCurrent Current frame
     * @return Motion vector texture
     */
    ID3D11Texture2D* Calculate(
        ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* framePrev,
        ID3D11ShaderResourceView* frameCurrent
    );
    
    /**
     * Get motion vector SRV
     */
    ID3D11ShaderResourceView* GetMotionVectorsSRV() const { return m_MotionVectorsSRV; }

private:
    bool CreateShader();
    
    ID3D11Device* m_Device = nullptr;
    ID3D11ComputeShader* m_OpticalFlowCS = nullptr;
    ID3D11Texture2D* m_MotionVectors = nullptr;
    ID3D11ShaderResourceView* m_MotionVectorsSRV = nullptr;
    ID3D11UnorderedAccessView* m_MotionVectorsUAV = nullptr;
    
    UINT m_Width = 0;
    UINT m_Height = 0;
};

/**
 * Factory function to create frame generator
 */
std::unique_ptr<IFrameGenerator> CreateFrameGenerator(Backend backend);

} // namespace FrameGen
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_FRAME_GENERATOR_H
