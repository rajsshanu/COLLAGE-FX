#include "CudaBackend.h"
#include "Log.h"
#include "MixedMediaPTX.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace mm {
namespace {

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;

constexpr CUresult CUDA_SUCCESS = 0;

typedef CUresult(__stdcall* PFN_cuInit)(unsigned int);
typedef CUresult(__stdcall* PFN_cuDeviceGet)(CUdevice*, int);
typedef CUresult(__stdcall* PFN_cuDeviceGetName)(char*, int, CUdevice);
typedef CUresult(__stdcall* PFN_cuCtxGetCurrent)(void**);
typedef CUresult(__stdcall* PFN_cuCtxSetCurrent)(void*);
typedef CUresult(__stdcall* PFN_cuCtxGetDevice)(CUdevice*);
typedef CUresult(__stdcall* PFN_cuCtxSynchronize)();
typedef CUresult(__stdcall* PFN_cuStreamSynchronize)(void*);
typedef CUresult(__stdcall* PFN_cuDevicePrimaryCtxRetain)(void**, CUdevice);
typedef CUresult(__stdcall* PFN_cuDevicePrimaryCtxRelease)(CUdevice);
typedef CUresult(__stdcall* PFN_cuMemAlloc)(CUdeviceptr*, size_t);
typedef CUresult(__stdcall* PFN_cuMemFree)(CUdeviceptr);
typedef CUresult(__stdcall* PFN_cuModuleLoadData)(void**, const void*);
typedef CUresult(__stdcall* PFN_cuModuleGetFunction)(void**, void*, const char*);
typedef CUresult(__stdcall* PFN_cuLaunchKernel)(void*, unsigned int, unsigned int,
                                                unsigned int, unsigned int, unsigned int,
                                                unsigned int, unsigned int, void*, void**,
                                                void**);
typedef CUresult(__stdcall* PFN_cuGetErrorName)(CUresult, const char**);
typedef CUresult(__stdcall* PFN_cuGetErrorString)(CUresult, const char**);

struct DriverApi
{
    HMODULE dll = nullptr;
    PFN_cuInit init = nullptr;
    PFN_cuDeviceGet deviceGet = nullptr;
    PFN_cuDeviceGetName deviceGetName = nullptr;
    PFN_cuCtxGetCurrent ctxGetCurrent = nullptr;
    PFN_cuCtxSetCurrent ctxSetCurrent = nullptr;
    PFN_cuCtxGetDevice ctxGetDevice = nullptr;
    PFN_cuCtxSynchronize ctxSynchronize = nullptr;
    PFN_cuStreamSynchronize streamSynchronize = nullptr;
    PFN_cuDevicePrimaryCtxRetain primaryCtxRetain = nullptr;
    PFN_cuDevicePrimaryCtxRelease primaryCtxRelease = nullptr;
    PFN_cuMemAlloc memAlloc = nullptr;
    PFN_cuMemFree memFree = nullptr;
    PFN_cuModuleLoadData moduleLoadData = nullptr;
    PFN_cuModuleGetFunction moduleGetFunction = nullptr;
    PFN_cuLaunchKernel launchKernel = nullptr;
    PFN_cuGetErrorName getErrorName = nullptr;
    PFN_cuGetErrorString getErrorString = nullptr;
};

struct SharedContext
{
    void* ctx = nullptr;
    void* module = nullptr;
    void* buildFn = nullptr;
    void* paperFn = nullptr;
    void* halftoneFn = nullptr;
    int device = 0;
    std::string deviceName;
};

DriverApi gDriver;
std::once_flag gDriverOnce;
bool gDriverLoaded = false;
std::string gDriverError;

std::mutex gSharedMutex;
std::vector<SharedContext*> gShared;

template <typename T>
bool resolve(T& fn, HMODULE dll, const char* name)
{
    fn = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!fn)
    {
        gDriverError = std::string("missing CUDA entry point: ") + name;
        return false;
    }
    return true;
}

bool loadDriver(std::string& error)
{
    std::call_once(gDriverOnce, []()
    {
        gDriver.dll = LoadLibraryA("nvcuda.dll");
        if (!gDriver.dll)
        {
            gDriverError = "nvcuda.dll not found";
            return;
        }

        bool ok = true;
        ok &= resolve(gDriver.init, gDriver.dll, "cuInit");
        ok &= resolve(gDriver.deviceGet, gDriver.dll, "cuDeviceGet");
        ok &= resolve(gDriver.deviceGetName, gDriver.dll, "cuDeviceGetName");
        ok &= resolve(gDriver.ctxGetCurrent, gDriver.dll, "cuCtxGetCurrent");
        ok &= resolve(gDriver.ctxSetCurrent, gDriver.dll, "cuCtxSetCurrent");
        ok &= resolve(gDriver.ctxGetDevice, gDriver.dll, "cuCtxGetDevice");
        ok &= resolve(gDriver.ctxSynchronize, gDriver.dll, "cuCtxSynchronize");
        ok &= resolve(gDriver.streamSynchronize, gDriver.dll, "cuStreamSynchronize");
        ok &= resolve(gDriver.primaryCtxRetain, gDriver.dll, "cuDevicePrimaryCtxRetain");
        resolve(gDriver.primaryCtxRelease, gDriver.dll, "cuDevicePrimaryCtxRelease");
        if (!resolve(gDriver.memAlloc, gDriver.dll, "cuMemAlloc_v2"))
            ok &= resolve(gDriver.memAlloc, gDriver.dll, "cuMemAlloc");
        if (!resolve(gDriver.memFree, gDriver.dll, "cuMemFree_v2"))
            ok &= resolve(gDriver.memFree, gDriver.dll, "cuMemFree");
        ok &= resolve(gDriver.moduleLoadData, gDriver.dll, "cuModuleLoadData");
        ok &= resolve(gDriver.moduleGetFunction, gDriver.dll, "cuModuleGetFunction");
        ok &= resolve(gDriver.launchKernel, gDriver.dll, "cuLaunchKernel");
        ok &= resolve(gDriver.getErrorName, gDriver.dll, "cuGetErrorName");
        resolve(gDriver.getErrorString, gDriver.dll, "cuGetErrorString");
        if (!ok)
            return;

        CUresult r = gDriver.init(0);
        if (r != CUDA_SUCCESS)
        {
            gDriverError = "cuInit failed";
            return;
        }
        gDriverLoaded = true;
    });

    if (!gDriverLoaded)
    {
        error = gDriverError.empty() ? "CUDA driver unavailable" : gDriverError;
        return false;
    }
    return true;
}

std::string cudaError(CUresult r)
{
    const char* name = nullptr;
    if (gDriver.getErrorName && gDriver.getErrorName(r, &name) == CUDA_SUCCESS && name)
        return name;
    return "CUDA error " + std::to_string(r);
}

bool loadModule(SharedContext& shared, std::string& error)
{
    CUresult r = gDriver.moduleLoadData(&shared.module, kMixedMediaPTX);
    if (r != CUDA_SUCCESS)
    {
        error = "cuModuleLoadData failed: " + cudaError(r);
        return false;
    }
    r = gDriver.moduleGetFunction(&shared.buildFn, shared.module, "mm_build_paper_kernel");
    if (r != CUDA_SUCCESS)
    {
        error = "mm_build_paper_kernel not found: " + cudaError(r);
        return false;
    }
    r = gDriver.moduleGetFunction(&shared.paperFn, shared.module, "mm_paper_main_kernel");
    if (r != CUDA_SUCCESS)
    {
        error = "mm_paper_main_kernel not found: " + cudaError(r);
        return false;
    }
    r = gDriver.moduleGetFunction(&shared.halftoneFn, shared.module, "mm_halftone_kernel");
    if (r != CUDA_SUCCESS)
    {
        error = "mm_halftone_kernel not found: " + cudaError(r);
        return false;
    }
    return true;
}

SharedContext* findOrCreateShared(void* ctx, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(gSharedMutex);
        for (SharedContext* item : gShared)
        {
            if (item->ctx == ctx)
                return item;
        }
    }

    SharedContext* shared = new SharedContext();
    shared->ctx = ctx;
    CUresult r = gDriver.ctxSetCurrent(ctx);
    if (r != CUDA_SUCCESS)
    {
        delete shared;
        error = "could not bind CUDA context: " + cudaError(r);
        return nullptr;
    }

    r = gDriver.ctxGetDevice(&shared->device);
    if (r == CUDA_SUCCESS)
    {
        char name[256] = {};
        if (gDriver.deviceGetName(name, sizeof(name), shared->device) == CUDA_SUCCESS)
            shared->deviceName = name;
    }

    if (!loadModule(*shared, error))
    {
        delete shared;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gSharedMutex);
        for (SharedContext* item : gShared)
        {
            if (item->ctx == ctx)
            {
                // Another thread won the race. Keep the module alive for the
                // process lifetime; dropping the duplicate is harmless.
                delete shared;
                return item;
            }
        }
        gShared.push_back(shared);
    }
    logMessage("CUDA ready on %s (device %d)",
               shared->deviceName.empty() ? "NVIDIA GPU" : shared->deviceName.c_str(),
               shared->device);
    return shared;
}

bool ensureReady(CudaRenderState& state, std::string& error)
{
    if (!loadDriver(error))
        return false;

    void* current = nullptr;
    CUresult r = gDriver.ctxGetCurrent(&current);
    if (r != CUDA_SUCCESS)
    {
        error = "cuCtxGetCurrent failed: " + cudaError(r);
        return false;
    }

    if (!current && state.ctx)
    {
        r = gDriver.ctxSetCurrent(state.ctx);
        if (r != CUDA_SUCCESS)
        {
            error = "could not rebind CUDA context: " + cudaError(r);
            return false;
        }
        current = state.ctx;
    }

    if (!current)
    {
        CUdevice device = 0;
        r = gDriver.deviceGet(&device, 0);
        if (r != CUDA_SUCCESS)
        {
            error = "no CUDA device: " + cudaError(r);
            return false;
        }
        r = gDriver.primaryCtxRetain(&current, device);
        if (r != CUDA_SUCCESS)
        {
            error = "could not retain CUDA primary context: " + cudaError(r);
            return false;
        }
        r = gDriver.ctxSetCurrent(current);
        if (r != CUDA_SUCCESS)
        {
            error = "could not bind CUDA primary context: " + cudaError(r);
            return false;
        }
        state.retainedPrimary = true;
    }

    if (!state.ctx)
        state.ctx = current;

    if (state.shared && state.ctx == current)
        return true;

    SharedContext* shared = findOrCreateShared(current, error);
    if (!shared)
        return false;

    state.shared = shared;
    state.ctx = current;
    if (!state.loggedReady)
    {
        state.loggedReady = true;
        logMessage("rendering on CUDA path (%s)",
                   shared->deviceName.empty() ? "NVIDIA GPU" : shared->deviceName.c_str());
    }
    return true;
}

bool ensurePieceBuffer(CudaRenderState& state, unsigned int capacity, std::string& error)
{
    if (capacity < 1)
        capacity = 1;
    if (capacity > MM_MAX_PAPER_PIECES)
        capacity = MM_MAX_PAPER_PIECES;
    if (state.pieceBuffer && state.pieceCapacity >= capacity)
        return true;

    if (state.pieceBuffer && gDriver.memFree)
    {
        gDriver.memFree((CUdeviceptr)state.pieceBuffer);
        state.pieceBuffer = nullptr;
        state.pieceCapacity = 0;
    }

    size_t bytes = (size_t)capacity * sizeof(PaperPiece);
    CUdeviceptr ptr = 0;
    CUresult r = gDriver.memAlloc(&ptr, bytes);
    if (r != CUDA_SUCCESS)
    {
        error = "CUDA piece buffer allocation failed: " + cudaError(r);
        return false;
    }
    state.pieceBuffer = (void*)ptr;
    state.pieceCapacity = capacity;
    return true;
}

bool validateImages(int width, int height, int inPitch, int outPitch,
                    const void* src, void* dst, std::string& error)
{
    if (width <= 0 || height <= 0)
    {
        error = "invalid image dimensions";
        return false;
    }
    if (!src || !dst)
    {
        error = "null CUDA image pointer";
        return false;
    }
    if (inPitch < width * 4 || outPitch < width * 4)
    {
        error = "CUDA row pitch is smaller than the image width";
        return false;
    }
    return true;
}

bool launchPaperBuild(CudaRenderState& state, const PaperParams& p, void* stream,
                      std::string& error)
{
    SharedContext* shared = static_cast<SharedContext*>(state.shared);
    unsigned int count = (unsigned int)(p.columns * p.rows);
    if (count < 1)
        count = 1;
    if (count > MM_MAX_PAPER_PIECES)
        count = MM_MAX_PAPER_PIECES;

    if (!ensurePieceBuffer(state, count, error))
        return false;

    CUdeviceptr pieces = (CUdeviceptr)state.pieceBuffer;
    void* args[] = { const_cast<PaperParams*>(&p), &pieces };
    unsigned int blocks = (count + 127) / 128;
    CUresult r = gDriver.launchKernel(shared->buildFn, blocks, 1, 1, 128, 1, 1,
                                      0, stream, args, nullptr);
    if (r != CUDA_SUCCESS)
    {
        error = "paper build kernel launch failed: " + cudaError(r);
        return false;
    }
    return true;
}

bool finish(CudaRenderState& state, void* stream, std::string& error)
{
    (void)state;
    if (stream)
        return true;
    CUresult r = gDriver.ctxSynchronize();
    if (r != CUDA_SUCCESS)
    {
        error = "CUDA context synchronisation failed: " + cudaError(r);
        return false;
    }
    return true;
}

} // namespace

bool cudaDriverAvailable()
{
    std::string error;
    return loadDriver(error);
}

bool cudaRenderPaper(CudaRenderState& state, const PaperParams& params,
                     const void* srcDevice, void* dstDevice, void* stream,
                     std::string& error)
{
    if (state.failed)
    {
        error = "CUDA state previously failed";
        return false;
    }
    if (!validateImages(params.width, params.height, params.srcPitch, params.outPitch,
                        srcDevice, dstDevice, error))
        return false;
    if (params.columns < 1 || params.rows < 1 ||
        params.columns * params.rows > MM_MAX_PAPER_PIECES)
    {
        error = "invalid paper grid";
        return false;
    }
    if (!ensureReady(state, error))
    {
        state.failed = true;
        return false;
    }

    SharedContext* shared = static_cast<SharedContext*>(state.shared);
    CUresult r = gDriver.ctxSetCurrent(shared->ctx);
    if (r != CUDA_SUCCESS)
    {
        error = "could not bind CUDA context: " + cudaError(r);
        return false;
    }

    if (!launchPaperBuild(state, params, stream, error))
    {
        state.failed = true;
        return false;
    }

    CUdeviceptr src = (CUdeviceptr)srcDevice;
    CUdeviceptr dst = (CUdeviceptr)dstDevice;
    CUdeviceptr pieces = (CUdeviceptr)state.pieceBuffer;
    void* args[] = { const_cast<PaperParams*>(&params), &src, &dst, &pieces };
    unsigned int bx = (unsigned int)((params.width + 15) / 16);
    unsigned int by = (unsigned int)((params.height + 15) / 16);
    r = gDriver.launchKernel(shared->paperFn, bx, by, 1, 16, 16, 1,
                             0, stream, args, nullptr);
    if (r != CUDA_SUCCESS)
    {
        error = "paper kernel launch failed: " + cudaError(r);
        state.failed = true;
        return false;
    }

    if (!finish(state, stream, error))
    {
        state.failed = true;
        return false;
    }
    return true;
}

bool cudaRenderHalftone(CudaRenderState& state, const HalftoneParams& params,
                        const void* srcDevice, void* dstDevice, void* stream,
                        std::string& error)
{
    if (state.failed)
    {
        error = "CUDA state previously failed";
        return false;
    }
    if (!validateImages(params.width, params.height, params.srcPitch, params.outPitch,
                        srcDevice, dstDevice, error))
        return false;
    if (!ensureReady(state, error))
    {
        state.failed = true;
        return false;
    }

    SharedContext* shared = static_cast<SharedContext*>(state.shared);
    CUresult r = gDriver.ctxSetCurrent(shared->ctx);
    if (r != CUDA_SUCCESS)
    {
        error = "could not bind CUDA context: " + cudaError(r);
        return false;
    }

    CUdeviceptr src = (CUdeviceptr)srcDevice;
    CUdeviceptr dst = (CUdeviceptr)dstDevice;
    void* args[] = { const_cast<HalftoneParams*>(&params), &src, &dst };
    unsigned int bx = (unsigned int)((params.width + 15) / 16);
    unsigned int by = (unsigned int)((params.height + 15) / 16);
    r = gDriver.launchKernel(shared->halftoneFn, bx, by, 1, 16, 16, 1,
                             0, stream, args, nullptr);
    if (r != CUDA_SUCCESS)
    {
        error = "halftone kernel launch failed: " + cudaError(r);
        state.failed = true;
        return false;
    }

    if (!finish(state, stream, error))
    {
        state.failed = true;
        return false;
    }
    return true;
}

void cudaDestroy(CudaRenderState& state)
{
    std::string ignored;
    if (!loadDriver(ignored))
    {
        state.shared = nullptr;
        state.ctx = nullptr;
        state.pieceBuffer = nullptr;
        state.pieceCapacity = 0;
        return;
    }
    if (state.ctx && gDriver.ctxSetCurrent)
        gDriver.ctxSetCurrent(state.ctx);
    if (state.pieceBuffer && gDriver.memFree)
    {
        if (gDriver.ctxSynchronize)
            gDriver.ctxSynchronize();
        gDriver.memFree((CUdeviceptr)state.pieceBuffer);
        state.pieceBuffer = nullptr;
    }
    state.shared = nullptr;
    state.ctx = nullptr;
    state.pieceCapacity = 0;
}

} // namespace mm
