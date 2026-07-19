//
// Minimal HIP runtime wrapper — see header.
//

#include "HipDriver.h"

#include <cstdio>
#include <cstdlib>
#include <string>
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
    void* loadHip() { return loadLib("amdhip64.dll"); }
}
#else
#  include <dlfcn.h>
namespace {
    void* sym(void* lib, const char* name) { return dlsym(lib, name); }
    void closeLib(void* lib) { dlclose(lib); }

    // Load libamdhip64, preferring the alternatives-canonical ROCm
    // (/opt/rocm, the update-alternatives target) and then $ROCM_PATH over a
    // bare soname. Before loading hip from a chosen dir, PIN that dir's
    // libhsa-runtime with RTLD_GLOBAL so hip's transitive HSA dependency
    // binds to it by soname instead of being re-resolved through
    // LD_LIBRARY_PATH. On a box with mixed ROCm installs (or a stale/poisoned
    // LD_LIBRARY_PATH) the default search can otherwise pull a different,
    // possibly-broken libhsa and crash deep inside the runtime at code-object
    // load. On a normal single-install box this pins the same libhsa hip
    // would load anyway — harmless. Best-effort: any dlopen that fails just
    // falls through to the next candidate.
    void* loadHipFromDir(const std::string& dir) {
        std::string hsa = dir + "/libhsa-runtime64.so.1";
        std::string hip = dir + "/libamdhip64.so";
        dlopen(hsa.c_str(), RTLD_NOW | RTLD_GLOBAL);   // pin canonical HSA
        return dlopen(hip.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    void* loadHip() {
        // 1. Canonical selected ROCm (update-alternatives target).
        if (void* h = loadHipFromDir("/opt/rocm/lib")) return h;
        // 2. $ROCM_PATH/lib, if set and distinct.
        if (const char* rp = std::getenv("ROCM_PATH")) {
            if (void* h = loadHipFromDir(std::string(rp) + "/lib")) return h;
        }
        // 3. Trust the dynamic loader (LD_LIBRARY_PATH / ld.so cache).
        for (const char* n : {"libamdhip64.so", "libamdhip64.so.7"}) {
            if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
        }
        return nullptr;
    }
}
#endif

namespace cajeta {
namespace xpu {
namespace amd {

namespace {
constexpr int hipSuccess = 0;
} // namespace

// Resolved HIP entry points. HIP exports plain C symbols (no size-versioning).
struct HipDriver::Api {
    void* lib = nullptr;
    int (*hipInit)(unsigned) = nullptr;
    int (*hipGetDeviceCount)(int*) = nullptr;
    int (*hipSetDevice)(int) = nullptr;
    int (*hipModuleLoadData)(void**, const void*) = nullptr;
    int (*hipModuleGetFunction)(void**, void*, const char*) = nullptr;
    int (*hipMalloc)(void**, size_t) = nullptr;
    int (*hipMemcpyHtoD)(void*, const void*, size_t) = nullptr;
    int (*hipMemcpyDtoH)(void*, void*, size_t) = nullptr;
    int (*hipFree)(void*) = nullptr;
    int (*hipModuleLaunchKernel)(void*, unsigned, unsigned, unsigned,
                                 unsigned, unsigned, unsigned, unsigned,
                                 void*, void**, void**) = nullptr;
    int (*hipDeviceSynchronize)() = nullptr;

    bool resolveAll() {
        lib = loadHip();
        if (!lib) return false;
        auto bind = [&](auto& fp, const char* name) {
            fp = reinterpret_cast<std::remove_reference_t<decltype(fp)>>(
                sym(lib, name));
            return fp != nullptr;
        };
        return bind(hipInit, "hipInit")
            && bind(hipGetDeviceCount, "hipGetDeviceCount")
            && bind(hipSetDevice, "hipSetDevice")
            && bind(hipModuleLoadData, "hipModuleLoadData")
            && bind(hipModuleGetFunction, "hipModuleGetFunction")
            && bind(hipMalloc, "hipMalloc")
            && bind(hipMemcpyHtoD, "hipMemcpyHtoD")
            && bind(hipMemcpyDtoH, "hipMemcpyDtoH")
            && bind(hipFree, "hipFree")
            && bind(hipModuleLaunchKernel, "hipModuleLaunchKernel")
            && bind(hipDeviceSynchronize, "hipDeviceSynchronize");
    }
};

namespace {
bool ok(int rc, const char* what) {
    if (rc != hipSuccess) {
        std::fprintf(stderr, "cajeta.xpu.amd: %s failed (hipError=%d)\n",
                     what, rc);
        return false;
    }
    return true;
}
} // namespace

bool HipDriver::available() {
    Api api;
    if (!api.resolveAll()) {
        if (api.lib) closeLib(api.lib);
        return false;
    }
    bool good = api.hipInit(0) == hipSuccess;
    int count = 0;
    good = good && api.hipGetDeviceCount(&count) == hipSuccess && count > 0;
    closeLib(api.lib);
    return good;
}

HipDriver::~HipDriver() {
    if (api) {
        if (api->lib) closeLib(api->lib);
        delete api;
    }
}

bool HipDriver::init() {
    if (initialized) return true;
    // A prior failed init() may have left an Api (and an open dlopen handle);
    // release it before retrying so `api` isn't overwritten and leaked.
    if (api) {
        if (api->lib) closeLib(api->lib);
        delete api;
        api = nullptr;
    }
    api = new Api();
    if (!api->resolveAll()) {
        std::fprintf(stderr, "cajeta.xpu.amd: could not load libamdhip64\n");
        return false;
    }
    if (!ok(api->hipInit(0), "hipInit")) return false;
    if (!ok(api->hipSetDevice(device), "hipSetDevice")) return false;
    initialized = true;
    return true;
}

HipModule HipDriver::loadModule(const void* image, std::size_t) {
    void* m = nullptr;
    if (!ok(api->hipModuleLoadData(&m, image), "hipModuleLoadData")) return nullptr;
    return m;
}

HipFunction HipDriver::getFunction(HipModule m, const char* name) {
    void* f = nullptr;
    if (!ok(api->hipModuleGetFunction(&f, m, name), "hipModuleGetFunction"))
        return nullptr;
    return f;
}

HipDevicePtr HipDriver::alloc(std::size_t bytes) {
    void* p = nullptr;
    if (!ok(api->hipMalloc(&p, bytes), "hipMalloc")) return nullptr;
    return p;
}

bool HipDriver::memcpyHtoD(HipDevicePtr dst, const void* src, std::size_t n) {
    return ok(api->hipMemcpyHtoD(dst, src, n), "hipMemcpyHtoD");
}

bool HipDriver::memcpyDtoH(void* dst, HipDevicePtr src, std::size_t n) {
    return ok(api->hipMemcpyDtoH(dst, src, n), "hipMemcpyDtoH");
}

void HipDriver::free(HipDevicePtr p) {
    if (p) api->hipFree(p);
}

bool HipDriver::launch(HipFunction f, unsigned gridX, unsigned blockX,
                       void** kernelParams, unsigned sharedMemBytes) {
    // hipModuleLaunchKernel mirrors cuLaunchKernel: gridDimX is the number of
    // BLOCKS (work-groups), blockDimX the threads per block.
    return ok(api->hipModuleLaunchKernel(f, gridX, 1, 1, blockX, 1, 1,
                                         sharedMemBytes, /*stream=*/nullptr,
                                         kernelParams, /*extra=*/nullptr),
              "hipModuleLaunchKernel");
}

bool HipDriver::synchronize() {
    return ok(api->hipDeviceSynchronize(), "hipDeviceSynchronize");
}

} // namespace amd
} // namespace xpu
} // namespace cajeta
