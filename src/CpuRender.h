#pragma once

#include "EffectMath.h"

namespace mm {

// CPU reference / fallback renderers. `dst` and `src` point at the lower-left
// pixel of their OFX bounds; pitch values are in floats and may be negative.
void renderPaperCpu(const PaperParams& params, const float* src, float* dst,
                    int threadCount = 0);

void renderHalftoneCpu(const HalftoneParams& params, const float* src, float* dst,
                       int threadCount = 0);

} // namespace mm
