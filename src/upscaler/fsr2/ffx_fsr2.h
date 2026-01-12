// Minimal FSR2 Interface for compilation support
#pragma once

#include <d3d12.h>
#include <cstdint>

#define FFX_FSR2_CONTEXT_SIZE 16384 // Approximate

typedef struct FfxFsr2Context {
    uint8_t data[FFX_FSR2_CONTEXT_SIZE];
} FfxFsr2Context;

typedef struct FfxFsr2ContextDescription {
    uint32_t flags;
    float maxRenderSize[2];
    float displaySize[2];
    void* callbacks;
    ID3D12Device* device;
} FfxFsr2ContextDescription;

typedef struct FfxFsr2DispatchDescription {
    ID3D12CommandList* commandList;
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* motionVectors;
    ID3D12Resource* exposure;
    ID3D12Resource* reactive;
    ID3D12Resource* transparencyAndComposition;
    ID3D12Resource* output;
    float jitterOffset[2];
    float motionVectorScale[2];
    float renderSize[2];
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;
} FfxFsr2DispatchDescription;

// Interface Functions (To be linked against ffx_fsr2_api_x64.lib or simulated)
// Since we don't have the lib, we implement stubs if needed, or rely on external linking.
// We will simply declare them here.

#ifdef __cplusplus
extern "C" {
#endif

// We'll define function pointers for manual loading or Stub them.
// For the purpose of this task (Code Generation), we assume linking is handled or we use a header-only impl.
// But FSR2 is not header-only.
// We will stub them to return "Success" code (0).

typedef int32_t FfxErrorCode;
#define FFX_OK 0

FfxErrorCode ffxFsr2ContextCreate(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription);
FfxErrorCode ffxFsr2ContextDispatch(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription);
FfxErrorCode ffxFsr2ContextDestroy(FfxFsr2Context* context);

#ifdef __cplusplus
}
#endif
