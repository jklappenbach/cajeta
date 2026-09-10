// Minimal CUDA Driver API wrapper: nvcuda is resolved at first use rather than
// linked, and the CUDA types are redeclared locally, so a machine with no NVIDIA
// driver simply has no NVIDIA device and the build needs no cuda.h or cuda.lib.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cajeta {
namespace xpu {
namespace nvidia {

    // ABI-matching stand-ins for cuda.h's handles, which are not included here.
    using CudaModule = void*;
    using CudaFunction = void*;
    using CudaDevicePtr = unsigned long long;

    // A process-wide context on device 0 plus the resolved entry points; every
    // method returns false on a driver error, reporting it to stderr.
    class CudaDriver {
    public:
        // True iff nvcuda loads, cuInit succeeds and a device exists. No init() needed.
        static bool available();

        CudaDriver() = default;
        ~CudaDriver();
        CudaDriver(const CudaDriver&) = delete;
        CudaDriver& operator=(const CudaDriver&) = delete;

        // Resolve nvcuda, cuInit, get device 0, create a context. Idempotent.
        bool init();

        CudaModule loadModule(const void* image, std::size_t len);
        CudaFunction getFunction(CudaModule m, const char* name);

        // Write a 32-bit value to a named module global: the spec-constant override
        // the runtime performs before a launch.
        bool setModuleGlobalI32(CudaModule m, const char* name, int32_t value);

        CudaDevicePtr alloc(std::size_t bytes);
        bool memcpyHtoD(CudaDevicePtr dst, const void* src, std::size_t bytes);
        bool memcpyDtoH(void* dst, CudaDevicePtr src, std::size_t bytes);
        void free(CudaDevicePtr p);

        // 1-D launch of gridX x blockX threads. `kernelParams` is the CUDA argv, an
        // array of pointers to each argument; `sharedMemBytes` sizes dynamic shared.
        bool launch(CudaFunction f, unsigned gridX, unsigned blockX,
                    void** kernelParams, unsigned sharedMemBytes = 0);

        bool synchronize();

    private:
        struct Api;     // resolved function pointers (defined in the .cpp)
        Api* api = nullptr;
        void* ctx = nullptr;       // CUcontext
        int device = 0;            // CUdevice
        bool initialized = false;
    };

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
