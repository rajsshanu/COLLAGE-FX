#include "CpuRender.h"

#include <algorithm>
#include <thread>
#include <vector>

namespace mm {
namespace {

int chooseThreadCount(int requested)
{
    if (requested > 0)
        return std::max(1, requested);
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 4;
    return (int)std::max(1u, std::min(hw, 16u));
}

template <typename RowFn>
void renderRows(int height, int threadCount, RowFn fn)
{
    const int threads = std::min(chooseThreadCount(threadCount), std::max(1, height));
    if (threads <= 1)
    {
        fn(0, height);
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve((size_t)threads - 1);
    int rowsPerThread = (height + threads - 1) / threads;
    for (int t = 1; t < threads; ++t)
    {
        int begin = t * rowsPerThread;
        int end = std::min(height, begin + rowsPerThread);
        if (begin >= end)
            break;
        workers.emplace_back(fn, begin, end);
    }
    fn(0, std::min(height, rowsPerThread));
    for (std::thread& worker : workers)
        worker.join();
}

} // namespace

void renderPaperCpu(const PaperParams& params, const float* src, float* dst,
                    int threadCount)
{
    int count = params.columns * params.rows;
    count = std::max(0, std::min(count, MM_MAX_PAPER_PIECES));
    std::vector<PaperPiece> pieces((size_t)std::max(count, 1));

    for (int i = 0; i < count; ++i)
    {
        int col = i % params.columns;
        int row = i / params.columns;
        mm_build_paper_piece(params, col, row, pieces[(size_t)i]);
    }

    renderRows(params.height, threadCount, [&](int begin, int end)
    {
        for (int y = begin; y < end; ++y)
        {
            for (int x = 0; x < params.width; ++x)
            {
                Vec4 c = mm_paper_pixel(params, pieces.data(), src, x, y);
                float* out = dst + (long long)y * params.outPitch + (long long)x * 4;
                out[0] = c.x;
                out[1] = c.y;
                out[2] = c.z;
                out[3] = c.w;
            }
        }
    });
}

void renderHalftoneCpu(const HalftoneParams& params, const float* src, float* dst,
                       int threadCount)
{
    renderRows(params.height, threadCount, [&](int begin, int end)
    {
        for (int y = begin; y < end; ++y)
        {
            for (int x = 0; x < params.width; ++x)
            {
                Vec4 c = mm_halftone_pixel(params, src, x, y);
                float* out = dst + (long long)y * params.outPitch + (long long)x * 4;
                out[0] = c.x;
                out[1] = c.y;
                out[2] = c.z;
                out[3] = c.w;
            }
        }
    });
}

} // namespace mm
