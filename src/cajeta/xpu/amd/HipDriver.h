//
// Minimal HIP runtime wrapper — dlopen'd libamdhip64, no build-time ROCm dep.
//
// The AMD twin of CudaDriver (cajeta-amd.md §1, Driver seam). Same contract:
// resolve the handful of HIP entry points the XPU runtime needs at first use
// via dlopen/dlsym, defining the HIP handle types locally so the build needs
// neither hip_runtime.h nor libamdhip64 at link time. A box without ROCm
// simply has no AMD device — the binary still links and runs.
//
// Scope: load an hsaco and run a 1-D compute launch with stream-ordered
// buffers (the SAXPY end-to-end path). Broader surface layers on later.
//

#pragma once

#include <cstddef>

namespace cajeta {
namespace xpu {
namespace amd {

    // Opaque HIP handles (real definitions live in hip_runtime.h; these match
    // its ABI — module/function are pointers, hipDeviceptr_t is void*).
    using HipModule = void*;
    using HipFunction = void*;
    using HipDevicePtr = void*;

    // Device 0 bound via hipSetDevice, plus the resolved HIP entry points.
    // Construct once; methods return false on any HIP error (message to
    // stderr). `available()` is the cheap pre-check tests use to skip when no
    // GPU/ROCm is present.
    class HipDriver {
    public:
        // True iff libamdhip64 is loadable, hipInit succeeds, and at least one
        // device exists. Safe to call without a prior init().
        static bool available();

        HipDriver() = default;
        ~HipDriver();
        HipDriver(const HipDriver&) = delete;
        HipDriver& operator=(const HipDriver&) = delete;

        // Resolve libamdhip64, hipInit, select device 0. Idempotent.
        bool init();

        // Load an hsaco code object; fetch a kernel by its entry-symbol name.
        HipModule loadModule(const void* image, std::size_t len);
        HipFunction getFunction(HipModule m, const char* name);

        // Device memory + transfers.
        HipDevicePtr alloc(std::size_t bytes);
        bool memcpyHtoD(HipDevicePtr dst, const void* src, std::size_t bytes);
        bool memcpyDtoH(void* dst, HipDevicePtr src, std::size_t bytes);
        void free(HipDevicePtr p);

        // 1-D launch: grid x block threads. `kernelParams` is the HIP argv —
        // an array of pointers to each argument value. sharedMemBytes sizes
        // the kernel's dynamic (extern) LDS; default 0 for static-only kernels.
        bool launch(HipFunction f, unsigned gridX, unsigned blockX,
                    void** kernelParams, unsigned sharedMemBytes = 0);

        // Block until all prior device work finishes.
        bool synchronize();

    private:
        struct Api;     // resolved function pointers (defined in the .cpp)
        Api* api = nullptr;
        int device = 0;
        bool initialized = false;
    };

} // namespace amd
} // namespace xpu
} // namespace cajeta
