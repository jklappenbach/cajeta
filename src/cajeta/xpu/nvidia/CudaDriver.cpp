//
// Minimal CUDA Driver API wrapper — see header.
//

#include "CudaDriver.h"

#include <cstdio>
#include <type_traits>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
namespace {
    void* loadLib(const char* name) {
        return reinterpret_cast<void*>(LoadLibraryA(name));
    }
    void* sym(void* lib, const char* name) {
        return reinterpret_cast<void*>(
            GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
    }
    void closeLib(void* lib) { FreeLibrary(reinterpret_cast<HMODULE>(lib)); }
    const char* kCudaLib = "nvcuda.dll";
}
#else
#  include <dlfcn.h>
namespace {
    void* loadLib(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
    void* sym(void* lib, const char* name) { return dlsym(lib, name); }
    void closeLib(void* lib) { dlclose(lib); }
    const char* kCudaLib = "libcuda.so.1";
}
#endif

namespace cajeta {
namespace xpu {
namespace nvidia {

namespace {
constexpr int CUDA_SUCCESS = 0;
} // namespace

// Resolved driver entry points. The driver API exposes size-versioned
// symbols (cuMemAlloc_v2, …); we bind those explicitly.
struct CudaDriver::Api {
    void* lib = nullptr;
    int (*cuInit)(unsigned) = nullptr;
    int (*cuDeviceGetCount)(int*) = nullptr;
    int (*cuDeviceGet)(int*, int) = nullptr;
    int (*cuCtxCreate)(void**, unsigned, int) = nullptr;
    int (*cuModuleLoadData)(void**, const void*) = nullptr;
    int (*cuModuleGetFunction)(void**, void*, const char*) = nullptr;
    int (*cuMemAlloc)(CudaDevicePtr*, size_t) = nullptr;
    int (*cuMemcpyHtoD)(CudaDevicePtr, const void*, size_t) = nullptr;
    int (*cuMemcpyDtoH)(void*, CudaDevicePtr, size_t) = nullptr;
    int (*cuMemFree)(CudaDevicePtr) = nullptr;
    int (*cuLaunchKernel)(void*, unsigned, unsigned, unsigned,
                          unsigned, unsigned, unsigned, unsigned,
                          void*, void**, void**) = nullptr;
    int (*cuCtxSynchronize)() = nullptr;

    bool resolveAll() {
        lib = loadLib(kCudaLib);
        if (!lib) return false;
        auto bind = [&](auto& fp, const char* name) {
            fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(
                sym(lib, name));
            return fp != nullptr;
        };
        return bind(cuInit, "cuInit")
            && bind(cuDeviceGetCount, "cuDeviceGetCount")
            && bind(cuDeviceGet, "cuDeviceGet")
            && bind(cuCtxCreate, "cuCtxCreate_v2")
            && bind(cuModuleLoadData, "cuModuleLoadData")
            && bind(cuModuleGetFunction, "cuModuleGetFunction")
            && bind(cuMemAlloc, "cuMemAlloc_v2")
            && bind(cuMemcpyHtoD, "cuMemcpyHtoD_v2")
            && bind(cuMemcpyDtoH, "cuMemcpyDtoH_v2")
            && bind(cuMemFree, "cuMemFree_v2")
            && bind(cuLaunchKernel, "cuLaunchKernel")
            && bind(cuCtxSynchronize, "cuCtxSynchronize");
    }
};

namespace {
bool ok(int rc, const char* what) {
    if (rc != CUDA_SUCCESS) {
        std::fprintf(stderr, "cajeta.xpu.nvidia: %s failed (CUresult=%d)\n",
                     what, rc);
        return false;
    }
    return true;
}
} // namespace

bool CudaDriver::available() {
    Api api;
    if (!api.resolveAll()) {
        if (api.lib) closeLib(api.lib);
        return false;
    }
    bool good = api.cuInit(0) == CUDA_SUCCESS;
    int count = 0;
    good = good && api.cuDeviceGetCount(&count) == CUDA_SUCCESS && count > 0;
    closeLib(api.lib);
    return good;
}

CudaDriver::~CudaDriver() {
    // Context teardown is left to process exit — cuCtxDestroy ordering vs
    // the dlopen'd lib is fiddly and the process is ending anyway.
    if (api) {
        if (api->lib) closeLib(api->lib);
        delete api;
    }
}

bool CudaDriver::init() {
    if (initialized) return true;
    api = new Api();
    if (!api->resolveAll()) {
        std::fprintf(stderr, "cajeta.xpu.nvidia: could not load %s\n", kCudaLib);
        return false;
    }
    if (!ok(api->cuInit(0), "cuInit")) return false;
    if (!ok(api->cuDeviceGet(&device, 0), "cuDeviceGet")) return false;
    if (!ok(api->cuCtxCreate(&ctx, 0, device), "cuCtxCreate")) return false;
    initialized = true;
    return true;
}

CudaModule CudaDriver::loadModule(const void* image, std::size_t) {
    void* m = nullptr;
    if (!ok(api->cuModuleLoadData(&m, image), "cuModuleLoadData")) return nullptr;
    return m;
}

CudaFunction CudaDriver::getFunction(CudaModule m, const char* name) {
    void* f = nullptr;
    if (!ok(api->cuModuleGetFunction(&f, m, name), "cuModuleGetFunction"))
        return nullptr;
    return f;
}

CudaDevicePtr CudaDriver::alloc(std::size_t bytes) {
    CudaDevicePtr p = 0;
    if (!ok(api->cuMemAlloc(&p, bytes), "cuMemAlloc")) return 0;
    return p;
}

bool CudaDriver::memcpyHtoD(CudaDevicePtr dst, const void* src, std::size_t n) {
    return ok(api->cuMemcpyHtoD(dst, src, n), "cuMemcpyHtoD");
}

bool CudaDriver::memcpyDtoH(void* dst, CudaDevicePtr src, std::size_t n) {
    return ok(api->cuMemcpyDtoH(dst, src, n), "cuMemcpyDtoH");
}

void CudaDriver::free(CudaDevicePtr p) {
    if (p) api->cuMemFree(p);
}

bool CudaDriver::launch(CudaFunction f, unsigned gridX, unsigned blockX,
                        void** kernelParams) {
    return ok(api->cuLaunchKernel(f, gridX, 1, 1, blockX, 1, 1,
                                  /*sharedMem=*/0, /*stream=*/nullptr,
                                  kernelParams, /*extra=*/nullptr),
              "cuLaunchKernel");
}

bool CudaDriver::synchronize() {
    return ok(api->cuCtxSynchronize(), "cuCtxSynchronize");
}

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
