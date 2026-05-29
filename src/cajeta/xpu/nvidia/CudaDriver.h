//
// Minimal CUDA Driver API wrapper — dlopen'd nvcuda, no build-time CUDA dep.
//
// CajetaXPU §1.1 / §9: libcajeta-xpu dlopens libcuda / nvcuda on demand so a
// machine without an NVIDIA driver simply has no NVIDIA device — the binary
// still links and runs. This wrapper embodies that: it resolves the handful
// of driver entry points the XPU runtime needs at first use via
// LoadLibrary/GetProcAddress, defining the CUDA types locally so the build
// needs neither cuda.h nor cuda.lib.
//
// Scope: just enough to load a cubin and run a 1-D compute launch with
// stream-ordered buffers (the SAXPY end-to-end path). Broader surface
// (events, async alloc, graphs) layers on later.
//

#pragma once

#include <cstddef>
#include <cstdint>

namespace cajeta {
namespace xpu {
namespace nvidia {

    // Opaque CUDA driver handles (real definitions live in cuda.h; these
    // match its ABI — pointers are void*, CUdeviceptr is u64 on 64-bit,
    // CUdevice/CUresult are int).
    using CudaModule = void*;
    using CudaFunction = void*;
    using CudaDevicePtr = unsigned long long;

    // A process-wide CUDA context bound to device 0, plus the resolved
    // driver entry points. Construct once; methods return false on any
    // driver error (with a message to stderr). `available()` is the cheap
    // pre-check tests use to skip when no GPU/driver is present.
    class CudaDriver {
    public:
        // True iff nvcuda is loadable, cuInit succeeds, and at least one
        // CUDA device exists. Safe to call without a prior init().
        static bool available();

        CudaDriver() = default;
        ~CudaDriver();
        CudaDriver(const CudaDriver&) = delete;
        CudaDriver& operator=(const CudaDriver&) = delete;

        // Resolve nvcuda, cuInit, get device 0, create a context. Idempotent.
        bool init();

        // Load a cubin/fatbin image; fetch a kernel by its PTX entry name.
        CudaModule loadModule(const void* image, std::size_t len);
        CudaFunction getFunction(CudaModule m, const char* name);

        // Device memory + transfers.
        CudaDevicePtr alloc(std::size_t bytes);
        bool memcpyHtoD(CudaDevicePtr dst, const void* src, std::size_t bytes);
        bool memcpyDtoH(void* dst, CudaDevicePtr src, std::size_t bytes);
        void free(CudaDevicePtr p);

        // 1-D launch: grid x block threads. `kernelParams` is the CUDA
        // argv — an array of pointers to each argument value.
        bool launch(CudaFunction f, unsigned gridX, unsigned blockX,
                    void** kernelParams);

        // Block until all prior work on the context finishes.
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
