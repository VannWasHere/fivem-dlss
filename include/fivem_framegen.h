#pragma once

/**
 * FiveM Frame Generation Mod
 * 
 * This header provides the public API for the Frame Generation mod.
 * It can be used by other mods to interact with the frame generation system.
 */

#ifndef FIVEM_FRAMEGEN_H
#define FIVEM_FRAMEGEN_H

#include <Windows.h>
#include <d3d11.h>
#include <cstdint>

#ifdef FIVEM_FRAMEGEN_EXPORTS
    #define FRAMEGEN_API __declspec(dllexport)
#else
    #define FRAMEGEN_API __declspec(dllimport)
#endif

namespace FiveMFrameGen {

// Version information
constexpr uint32_t VERSION_MAJOR = 1;
constexpr uint32_t VERSION_MINOR = 0;
constexpr uint32_t VERSION_PATCH = 0;

/**
 * Frame Generation Backend Types
 */
enum class Backend : uint32_t {
    None = 0,           // Frame generation disabled
    FSR3 = 1,           // AMD FSR 3 Frame Generation
    DLSS3 = 2,          // NVIDIA DLSS 3 (RTX 40 series only)
    OpticalFlow = 3     // Generic optical flow based
};

/**
 * Quality presets for frame generation
 */
enum class QualityPreset : uint32_t {
    Performance = 0,    // Fastest, lowest quality
    Balanced = 1,       // Balance between speed and quality
    Quality = 2         // Best quality, more GPU intensive
};

/**
 * Frame generation configuration
 */
struct Config {
    bool enabled = false;                           // Is frame gen enabled
    Backend backend = Backend::FSR3;                // Which backend to use
    QualityPreset quality = QualityPreset::Balanced;// Quality preset
    float targetFramerate = 60.0f;                  // Target output framerate
    bool showOverlay = true;                        // Show performance overlay
    bool hudLessMode = false;                       // Exclude HUD from interpolation
    float sharpness = 0.5f;                         // Sharpening strength (0-1)
};

/**
 * Performance statistics
 */
struct Stats {
    float baseFPS;          // Actual rendered FPS
    float outputFPS;        // Output FPS with frame gen
    float frameTimeMs;      // Frame time in milliseconds
    float gpuTimeMs;        // GPU time for frame gen
    uint64_t framesGenerated;// Total interpolated frames
    uint64_t framesMissed;   // Frames that couldn't be generated in time
};

/**
 * Initialize the frame generation system
 * 
 * @param device The D3D11 device
 * @param context The D3D11 device context
 * @param swapChain The DXGI swap chain
 * @return True if initialization succeeded
 */
FRAMEGEN_API bool Initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    IDXGISwapChain* swapChain
);

/**
 * Shutdown the frame generation system
 */
FRAMEGEN_API void Shutdown();

/**
 * Check if the system is initialized
 */
FRAMEGEN_API bool IsInitialized();

/**
 * Enable/disable frame generation
 */
FRAMEGEN_API void SetEnabled(bool enabled);

/**
 * Check if frame generation is enabled
 */
FRAMEGEN_API bool IsEnabled();

/**
 * Set the active backend
 * 
 * @param backend The backend to use
 * @return True if the backend is supported and was set
 */
FRAMEGEN_API bool SetBackend(Backend backend);

/**
 * Get the current backend
 */
FRAMEGEN_API Backend GetBackend();

/**
 * Set quality preset
 */
FRAMEGEN_API void SetQualityPreset(QualityPreset preset);

/**
 * Get current quality preset
 */
FRAMEGEN_API QualityPreset GetQualityPreset();

/**
 * Get full configuration
 */
FRAMEGEN_API const Config& GetConfig();

/**
 * Set full configuration
 */
FRAMEGEN_API void SetConfig(const Config& config);

/**
 * Get performance statistics
 */
FRAMEGEN_API const Stats& GetStats();

/**
 * Toggle the configuration overlay
 */
FRAMEGEN_API void ToggleOverlay();

/**
 * Check if a backend is supported on the current hardware
 * 
 * @param backend The backend to check
 * @return True if the backend is supported
 */
FRAMEGEN_API bool IsBackendSupported(Backend backend);

/**
 * Get a human-readable error message for the last error
 */
FRAMEGEN_API const char* GetLastError();

} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_H
