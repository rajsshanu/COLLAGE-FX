#pragma once

#include "EffectMath.h"

#include <mutex>
#include <string>

namespace mm {

// Per-instance CUDA state. The heavy module/function objects are shared per
// CUDA context; the small piece buffer and context binding belong to an
// instance so two instances can render concurrently.
struct CudaRenderState
{
    void* ctx = nullptr;
    void* shared = nullptr;
    void* pieceBuffer = nullptr;
    unsigned int pieceCapacity = 0;
    bool retainedPrimary = false;
    bool failed = false;
    bool loggedReady = false;
    std::mutex mutex;
};

bool cudaRenderPaper(CudaRenderState& state, const PaperParams& params,
                     const void* srcDevice, void* dstDevice, void* stream,
                     std::string& error);

bool cudaRenderHalftone(CudaRenderState& state, const HalftoneParams& params,
                        const void* srcDevice, void* dstDevice, void* stream,
                        std::string& error);

void cudaDestroy(CudaRenderState& state);

bool cudaDriverAvailable();

} // namespace mm
