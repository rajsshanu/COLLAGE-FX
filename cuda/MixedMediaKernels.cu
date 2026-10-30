// CUDA kernels for Mixed Media OFX.
//
// Compiled to PTX at build time by tools/compile_ptx.py with the NVRTC runtime
// that ships with DaVinci Resolve. The PTX is embedded in the plugin, so the
// installed .ofx needs no CUDA toolkit or CUDA runtime DLLs.
#include "EffectMath.h"

extern "C" __global__ void mm_build_paper_kernel(PaperParams p,
                                                 PaperPiece* __restrict__ pieces)
{
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    int count = p.columns * p.rows;
    if (index >= count)
        return;

    int col = index % p.columns;
    int row = index / p.columns;
    mm_build_paper_piece(p, col, row, pieces[index]);
}

extern "C" __global__ void mm_paper_main_kernel(PaperParams p,
                                                const float* __restrict__ src,
                                                float* __restrict__ dst,
                                                const PaperPiece* __restrict__ pieces)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || y >= p.height)
        return;

    Vec4 out = mm_paper_pixel(p, pieces, src, x, y);
    float* o = dst + (long long)y * p.outPitch + x * 4;
    o[0] = out.x;
    o[1] = out.y;
    o[2] = out.z;
    o[3] = out.w;
}

extern "C" __global__ void mm_halftone_kernel(HalftoneParams p,
                                              const float* __restrict__ src,
                                              float* __restrict__ dst)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.width || y >= p.height)
        return;

    Vec4 out = mm_halftone_pixel(p, src, x, y);
    float* o = dst + (long long)y * p.outPitch + x * 4;
    o[0] = out.x;
    o[1] = out.y;
    o[2] = out.z;
    o[3] = out.w;
}
