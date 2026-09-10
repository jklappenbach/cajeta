// Minimal HIP runtime wrapper — see header.

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

    // Loads libamdhip64 from `dir`, first pinning that dir's libhsa-runtime with
    // RTLD_GLOBAL so hip's transitive HSA binds by soname rather than through
    // LD_LIBRARY_PATH (a mixed ROCm install otherwise crashes at code-object load).
    void* loadHipFromDir(const std::string& dir) {
        std::string hsa = dir + "/libhsa-runtime64.so.1";
        std::string hip = dir + "/libamdhip64.so";
        dlopen(hsa.c_str(), RTLD_NOW | RTLD_GLOBAL);
        return dlopen(hip.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    void* loadHip() {
        if (void* h = loadHipFromDir("/opt/rocm/lib")) return h;
        if (const char* rp = std::getenv("ROCM_PATH")) {
            if (void* h = loadHipFromDir(std::string(rp) + "/lib")) return h;
        }
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
    // gridX counts BLOCKS (work-groups), blockX threads per block, as cuLaunchKernel does.
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
