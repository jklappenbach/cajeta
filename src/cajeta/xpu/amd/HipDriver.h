// Minimal HIP runtime wrapper — libamdhip64 is dlopen'd at first use, so the
// build needs neither hip_runtime.h nor ROCm; a box without ROCm still links.

#pragma once

#include <cstddef>

namespace cajeta {
namespace xpu {
namespace amd {

    // Opaque handles matching hip_runtime.h's ABI: all three are pointers.
    using HipModule = void*;
    using HipFunction = void*;
    using HipDevicePtr = void*;

    // Device 0 plus the resolved entry points. Every method returns false on a
    // HIP error, after writing a message to stderr.
    class HipDriver {
    public:
        // True iff libamdhip64 loads, hipInit succeeds and a device exists.
        static bool available();

        HipDriver() = default;
        ~HipDriver();
        HipDriver(const HipDriver&) = delete;
        HipDriver& operator=(const HipDriver&) = delete;

        // Resolve libamdhip64, hipInit, select device 0. Idempotent.
        bool init();

        HipModule loadModule(const void* image, std::size_t len);
        HipFunction getFunction(HipModule m, const char* name);

        HipDevicePtr alloc(std::size_t bytes);
        bool memcpyHtoD(HipDevicePtr dst, const void* src, std::size_t bytes);
        bool memcpyDtoH(void* dst, HipDevicePtr src, std::size_t bytes);
        void free(HipDevicePtr p);

        // 1-D launch of grid x block threads. `kernelParams` is the HIP argv, an
        // array of pointers to argument values; sharedMemBytes sizes dynamic LDS.
        bool launch(HipFunction f, unsigned gridX, unsigned blockX,
                    void** kernelParams, unsigned sharedMemBytes = 0);

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
