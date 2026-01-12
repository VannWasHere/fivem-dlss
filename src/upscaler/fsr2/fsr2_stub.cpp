
#include "ffx_fsr2.h"

// Stub implementation to satisfy linker if lib is missing
extern "C" {
    // Return Error to force fallback to Bilinear Shader
    FfxErrorCode ffxFsr2ContextCreate(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription) { return -1; }
    FfxErrorCode ffxFsr2ContextDispatch(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription) { return -1; }
    FfxErrorCode ffxFsr2ContextDestroy(FfxFsr2Context* context) { return 0; }
}
