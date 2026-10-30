// CPU/CUDA parity test for the two Mixed Media kernels.
// Uses nvcuda.dll directly, exactly like the OFX plugin.
#include "../src/CpuRender.h"
#include "../src/CudaBackend.h"
#include "../src/EffectMath.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
constexpr CUresult CUDA_SUCCESS = 0;

typedef CUresult(__stdcall* PFN_cuInit)(unsigned int);
typedef CUresult(__stdcall* PFN_cuDeviceGet)(CUdevice*, int);
typedef CUresult(__stdcall* PFN_cuDevicePrimaryCtxRetain)(void**, CUdevice);
typedef CUresult(__stdcall* PFN_cuCtxSetCurrent)(void*);
typedef CUresult(__stdcall* PFN_cuMemAlloc)(CUdeviceptr*, size_t);
typedef CUresult(__stdcall* PFN_cuMemFree)(CUdeviceptr);
typedef CUresult(__stdcall* PFN_cuMemcpyHtoD)(CUdeviceptr, const void*, size_t);
typedef CUresult(__stdcall* PFN_cuMemcpyDtoH)(void*, CUdeviceptr, size_t);
typedef CUresult(__stdcall* PFN_cuCtxSynchronize)();
typedef CUresult(__stdcall* PFN_cuGetErrorName)(CUresult, const char**);

struct Driver
{
    HMODULE dll = nullptr;
    PFN_cuInit init = nullptr;
    PFN_cuDeviceGet deviceGet = nullptr;
    PFN_cuDevicePrimaryCtxRetain retain = nullptr;
    PFN_cuCtxSetCurrent setCurrent = nullptr;
    PFN_cuMemAlloc memAlloc = nullptr;
    PFN_cuMemFree memFree = nullptr;
    PFN_cuMemcpyHtoD htoD = nullptr;
    PFN_cuMemcpyDtoH dtoH = nullptr;
    PFN_cuCtxSynchronize sync = nullptr;
    PFN_cuGetErrorName errorName = nullptr;

    bool load()
    {
        dll = LoadLibraryA("nvcuda.dll");
        if (!dll) return false;
        init = (PFN_cuInit)GetProcAddress(dll, "cuInit");
        deviceGet = (PFN_cuDeviceGet)GetProcAddress(dll, "cuDeviceGet");
        retain = (PFN_cuDevicePrimaryCtxRetain)GetProcAddress(dll, "cuDevicePrimaryCtxRetain");
        setCurrent = (PFN_cuCtxSetCurrent)GetProcAddress(dll, "cuCtxSetCurrent");
        memAlloc = (PFN_cuMemAlloc)GetProcAddress(dll, "cuMemAlloc_v2");
        if (!memAlloc) memAlloc = (PFN_cuMemAlloc)GetProcAddress(dll, "cuMemAlloc");
        memFree = (PFN_cuMemFree)GetProcAddress(dll, "cuMemFree_v2");
        if (!memFree) memFree = (PFN_cuMemFree)GetProcAddress(dll, "cuMemFree");
        htoD = (PFN_cuMemcpyHtoD)GetProcAddress(dll, "cuMemcpyHtoD_v2");
        if (!htoD) htoD = (PFN_cuMemcpyHtoD)GetProcAddress(dll, "cuMemcpyHtoD");
        dtoH = (PFN_cuMemcpyDtoH)GetProcAddress(dll, "cuMemcpyDtoH_v2");
        if (!dtoH) dtoH = (PFN_cuMemcpyDtoH)GetProcAddress(dll, "cuMemcpyDtoH");
        sync = (PFN_cuCtxSynchronize)GetProcAddress(dll, "cuCtxSynchronize");
        errorName = (PFN_cuGetErrorName)GetProcAddress(dll, "cuGetErrorName");
        return init && deviceGet && retain && setCurrent && memAlloc && memFree &&
               htoD && dtoH && sync;
    }
};

static bool readRaw(const char* path, std::vector<float>& data)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    in.seekg(0, std::ios::beg);
    data.resize((size_t)n / sizeof(float));
    in.read((char*)data.data(), n);
    return in.good() || in.eof();
}

static bool compare(const char* name, const std::vector<float>& a,
                    const std::vector<float>& b, float tolerance)
{
    float maxDiff = 0.0f;
    double sum = 0.0;
    size_t maxIndex = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        float d = std::fabs(a[i] - b[i]);
        sum += d;
        if (d > maxDiff)
        {
            maxDiff = d;
            maxIndex = i;
        }
    }
    double mean = a.empty() ? 0.0 : sum / (double)a.size();
    std::printf("%s: max=%.6f mean=%.7f at %zu (cpu=%g gpu=%g)\n", name, maxDiff, mean,
                maxIndex, maxIndex < a.size() ? (double)a[maxIndex] : 0.0,
                maxIndex < b.size() ? (double)b[maxIndex] : 0.0);
    if (maxDiff > tolerance)
    {
        std::fprintf(stderr, "FAIL: %s differs by more than %g\n", name, tolerance);
        return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    const int width = argc > 1 ? std::atoi(argv[1]) : 512;
    const int height = argc > 2 ? std::atoi(argv[2]) : 512;
    const char* input = argc > 3 ? argv[3] : "tests/data/portrait.rgba";

    size_t count = (size_t)width * height * 4;
    std::vector<float> src(count);
    if (!readRaw(input, src) || src.size() < count)
    {
        std::printf("input %s not found; using synthetic test image\n", input);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                float* p = src.data() + ((size_t)y * width + x) * 4;
                float u = (float)x / (float)std::max(1, width - 1);
                float v = (float)y / (float)std::max(1, height - 1);
                p[0] = 0.15f + 0.7f * u;
                p[1] = 0.10f + 0.55f * v;
                p[2] = 0.08f + 0.30f * (1.0f - u);
                p[3] = 1.0f;
            }
        }
    }
    src.resize(count);

    Driver drv;
    if (!drv.load())
    {
        std::fprintf(stderr, "nvcuda.dll unavailable\n");
        return 77;
    }
    if (drv.init(0) != CUDA_SUCCESS)
    {
        std::fprintf(stderr, "cuInit failed\n");
        return 77;
    }
    CUdevice device = 0;
    if (drv.deviceGet(&device, 0) != CUDA_SUCCESS)
    {
        std::fprintf(stderr, "no CUDA device\n");
        return 77;
    }
    void* ctx = nullptr;
    if (drv.retain(&ctx, device) != CUDA_SUCCESS || drv.setCurrent(ctx) != CUDA_SUCCESS)
    {
        std::fprintf(stderr, "could not create CUDA context\n");
        return 77;
    }

    CUdeviceptr dsrc = 0, ddst = 0;
    if (drv.memAlloc(&dsrc, count * sizeof(float)) != CUDA_SUCCESS ||
        drv.memAlloc(&ddst, count * sizeof(float)) != CUDA_SUCCESS)
    {
        std::fprintf(stderr, "CUDA allocation failed\n");
        return 77;
    }
    if (drv.htoD(dsrc, src.data(), count * sizeof(float)) != CUDA_SUCCESS)
    {
        std::fprintf(stderr, "H2D copy failed\n");
        return 77;
    }

    const float pxScale = std::max(0.05f, (float)std::min(width, height) / 1080.0f);
    bool ok = true;

    {
        PaperParams p{};
        p.width = width; p.height = height;
        p.outPitch = width * 4; p.srcPitch = width * 4;
        p.srcW = width; p.srcH = height;
        p.columns = 6; p.rows = 5; p.seed = 7;
        p.missing = 0.16f;
        p.rotation = 5.0f * 3.14159265358979323846f / 180.0f;
        p.sizeVar = 0.18f; p.widthVar = 0.28f; p.heightVar = 0.28f;
        p.uvShift = 14.0f * pxScale;
        p.colorVar = 0.24f;
        p.tearSize = 16.0f * pxScale;
        p.chunkSize = 60.0f * pxScale;
        p.roughness = 0.58f;
        p.whiteEdge = 4.0f * pxScale;
        p.unevenness = 0.35f;
        p.shadow = 0.35f;
        p.bgImage = 1;
        p.bgR = 0.96f; p.bgG = 0.94f; p.bgB = 0.90f;
        p.pxScale = pxScale;

        std::vector<float> cpu(count, 0.0f);
        mm::renderPaperCpu(p, src.data(), cpu.data(), 1);

        mm::CudaRenderState state;
        std::string error;
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!mm::cudaRenderPaper(state, p, (const void*)dsrc, (void*)ddst, nullptr, error))
        {
            std::fprintf(stderr, "cudaRenderPaper failed: %s\n", error.c_str());
            ok = false;
        }
        else
        {
            auto t1 = std::chrono::high_resolution_clock::now();
            double firstMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            auto t2 = std::chrono::high_resolution_clock::now();
            if (!mm::cudaRenderPaper(state, p, (const void*)dsrc, (void*)ddst, nullptr, error))
            {
                std::fprintf(stderr, "warm cudaRenderPaper failed: %s\n", error.c_str());
                ok = false;
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            double warmMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
            std::printf("paper GPU: %.2f ms first, %.2f ms warm\n", firstMs, warmMs);
            std::vector<float> gpu(count, 0.0f);
            drv.dtoH(gpu.data(), ddst, count * sizeof(float));
            drv.sync();
            ok &= compare("paper", cpu, gpu, 2.0e-3f);
        }
        mm::cudaDestroy(state);
    }

    {
        HalftoneParams p{};
        p.width = width; p.height = height;
        p.outPitch = width * 4; p.srcPitch = width * 4;
        p.srcW = width; p.srcH = height;
        p.pattern = 0;
        p.frequency = 95.0f;
        p.roughness = 0.18f;
        p.fuzziness = 0.22f;
        p.paperFiber = 0.25f;
        p.inkTexture = 0.20f;
        p.inkDensity = 1.0f;
        p.pxScale = pxScale;
        p.useC = p.useM = p.useY = p.useK = 1;
        const float angles[4] = { 15.0f, 75.0f, 0.0f, 45.0f };
        for (int i = 0; i < 4; ++i)
            p.angle[i] = angles[i] * 3.14159265358979323846f / 180.0f;

        std::vector<float> cpu(count, 0.0f);
        mm::renderHalftoneCpu(p, src.data(), cpu.data(), 1);

        mm::CudaRenderState state;
        std::string error;
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!mm::cudaRenderHalftone(state, p, (const void*)dsrc, (void*)ddst, nullptr, error))
        {
            std::fprintf(stderr, "cudaRenderHalftone failed: %s\n", error.c_str());
            ok = false;
        }
        else
        {
            auto t1 = std::chrono::high_resolution_clock::now();
            double firstMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
            auto t2 = std::chrono::high_resolution_clock::now();
            if (!mm::cudaRenderHalftone(state, p, (const void*)dsrc, (void*)ddst, nullptr, error))
            {
                std::fprintf(stderr, "warm cudaRenderHalftone failed: %s\n", error.c_str());
                ok = false;
            }
            auto t3 = std::chrono::high_resolution_clock::now();
            double warmMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
            std::printf("halftone GPU: %.2f ms first, %.2f ms warm\n", firstMs, warmMs);
            std::vector<float> gpu(count, 0.0f);
            drv.dtoH(gpu.data(), ddst, count * sizeof(float));
            drv.sync();
            ok &= compare("halftone", cpu, gpu, 2.0e-3f);
        }
        mm::cudaDestroy(state);
    }

    drv.memFree(dsrc);
    drv.memFree(ddst);
    std::printf(ok ? "GPU parity OK\n" : "GPU parity FAILED\n");
    return ok ? 0 : 1;
}
