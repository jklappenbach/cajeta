//
// Shared GPU-hardware detection for the XPU test suite.
//
// All XPU tests compile into the single cajeta_test binary on every platform
// (the test glob is unconditional), so a test that drives a specific GPU
// backend MUST skip at runtime when that vendor's hardware is absent —
// otherwise it fails on any box lacking that GPU. This header is the single
// source of truth for "is backend X usable here?", delegating to the
// production drivers' no-throw `available()` probes:
//
//   * cudaAvailable()   — nvcuda loadable, cuInit ok, >=1 CUDA device
//   * hipAvailable()    — libamdhip64 loadable, hipInit ok, >=1 ROCm device
//   * vulkanAvailable() — libvulkan loadable, instance + compute queue exist
//
// Each probe is no-throw and returns false (never throws / aborts) when the
// runtime or device is missing, so it is safe to call unconditionally at the
// top of a test.
//
// Use the macros for the common "skip this whole test if the backend is
// absent" pattern; they expand to a GTEST_SKIP() with a clear reason:
//
//   TEST(MyAmdTest, runsOnDevice) {
//       CAJETA_SKIP_IF_NO_HIP();
//       ... launch a HIP kernel ...
//   }
//
// AOT artifact tests (.cubin via ptxas, .hsaco via ld.lld) gate on hardware
// too: the toolchain that produces a vendor code object ships with that
// vendor's stack, so "no device" is the right, uniform skip condition. (A
// generic host ld.lld being on PATH is NOT evidence an AMDGCN link will work —
// it cannot; it errors "incompatible with elf64-x86-64". Hence the hardware
// probe, not a tool-presence check.)
//

#pragma once

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/amd/HipDriver.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

namespace cajeta {
namespace xpu {
namespace test {

// Thin no-throw wrappers over each production driver's hardware probe. Kept as
// free functions so call sites read uniformly regardless of which driver
// namespace the backend lives in.
inline bool cudaAvailable()   { return ::cajeta::xpu::nvidia::CudaDriver::available(); }
inline bool hipAvailable()    { return ::cajeta::xpu::amd::HipDriver::available(); }
inline bool vulkanAvailable() { return ::cajeta::xpu::vulkan::VulkanDriver::available(); }

// "No Vulkan device" and "no Vulkan in this build" produce the SAME false from
// available(), and they call for opposite responses — plug in a GPU versus
// install the headers and rebuild. Say which.
inline const char* vulkanAbsenceReason() {
    return ::cajeta::xpu::vulkan::VulkanDriver::builtWithVulkan()
        ? "no Vulkan compute device available (this binary HAS Vulkan support)"
        : "this binary was built WITHOUT Vulkan support — <vulkan/vulkan.h> was "
          "absent at compile time, so the driver is a stub and every Vulkan "
          "test skips regardless of hardware; install libvulkan-dev and rebuild";
}

} // namespace test
} // namespace xpu
} // namespace cajeta

// Skip-the-current-test guards. Place at the top of a test body. The trailing
// `(void)0` lets the macro take a terminating semicolon like a statement.
#define CAJETA_SKIP_IF_NO_CUDA()                                              \
    do {                                                                      \
        if (!::cajeta::xpu::test::cudaAvailable())                            \
            GTEST_SKIP() << "no CUDA device/driver available";               \
    } while (0)

#define CAJETA_SKIP_IF_NO_HIP()                                              \
    do {                                                                      \
        if (!::cajeta::xpu::test::hipAvailable())                             \
            GTEST_SKIP() << "no ROCm/HIP device available";                  \
    } while (0)

#define CAJETA_SKIP_IF_NO_VULKAN()                                          \
    do {                                                                      \
        if (!::cajeta::xpu::test::vulkanAvailable())                          \
            GTEST_SKIP() << ::cajeta::xpu::test::vulkanAbsenceReason();      \
    } while (0)
