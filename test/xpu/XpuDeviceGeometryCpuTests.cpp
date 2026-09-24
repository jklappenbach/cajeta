// The geometry surface answers for the backend that will RUN the kernel, not
// for whatever silicon is plugged in (xpu-kernel-adaptor plan 1.9). On the cpu
// backend `Device` used to report the installed NVIDIA part -- 128 MPs x 4
// SIMDs, wave 32, 64K registers -- so `Linear.launchTargetBlocks()` computed a
// GPU-sized 256 for kernels that run on host threads, and `Autotune.deviceKey()`
// embedded a card the tuning does not depend on. The cpu backend now reports
// the host: its online cores (one block per worker), one scheduler per core,
// a wave of the host's SIMD lanes, physical RAM, integrated. What the host has
// no counterpart for answers 0 = unknown, never an invented GPU number.
#include "gtest/gtest.h"
#include <cstdint>
#include <cstdlib>
#if !defined(_WIN32)
#include <unistd.h>
#endif

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int32_t __cajeta_xpu_active_backend_id(void);
int64_t __cajeta_xpu_device_geometry(int32_t key);
int64_t __cajeta_xpu_device_memory_bytes(void);
}

namespace {
constexpr int32_t kBackendCpu = 3;   // CAJ_XPU_CPU
// CajetaXpuGeometryKey ordinals (cajeta_xpu_abi.h, append-only).
constexpr int32_t kMpCount = 0, kSimdsPerMp = 1, kWaveSize = 2,
                  kMaxThreadsPerBlock = 3, kLdsPerBlock = 4, kLdsPerMp = 6,
                  kBlocksPerMp = 7, kL2 = 8, kTotalMem = 9, kIntegrated = 10,
                  kRegsPerMp = 11, kThreadsPerMp = 12, kMaxGridX = 13,
                  kWavesPerSimdTarget = 15, kClockKHz = 16;

bool forceCpuBackend() {
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

int64_t hostCores() {
#if defined(_WIN32)
    return 0;   // asserted only where sysconf answers
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n < 1 ? 1 : n;
#endif
}
}  // namespace

TEST(XpuDeviceGeometryCpu, theCpuBackendAnswersForTheHostNotTheCard) {
    if (!forceCpuBackend()) GTEST_SKIP() << "could not select the cpu backend";
    // The worker cap bounds MP_COUNT; keep this test independent of the env.
    int64_t cap = 0;
    if (const char* e = std::getenv("CAJETA_XPU_CPU_WORKERS")) cap = std::atoll(e);

    int64_t mps = __cajeta_xpu_device_geometry(kMpCount);
    EXPECT_GE(mps, 1);
    if (int64_t cores = hostCores()) {
        int64_t want = cores;
        if (cap > 0 && want > cap) want = cap;
        if (want > 256) want = 256;
        EXPECT_EQ(mps, want) << "one block per worker: online cores, capped";
    }
    EXPECT_EQ(__cajeta_xpu_device_geometry(kSimdsPerMp), 1);

    // The host's SIMD lanes in 32-bit words: 4 (SSE2/NEON), 8 (AVX2), 16
    // (AVX-512), or 1 with no vector unit. Never a GPU's 32 or 64.
    int64_t wave = __cajeta_xpu_device_geometry(kWaveSize);
    EXPECT_TRUE(wave == 1 || wave == 4 || wave == 8 || wave == 16) << wave;

    EXPECT_EQ(__cajeta_xpu_device_geometry(kIntegrated), 1) << "the device IS the host";
    EXPECT_EQ(__cajeta_xpu_device_geometry(kTotalMem), __cajeta_xpu_device_memory_bytes())
        << "the geometry's memory and Device.memoryBytes() are one answer";
    EXPECT_GT(__cajeta_xpu_device_geometry(kMaxGridX), 0);
    EXPECT_EQ(__cajeta_xpu_device_geometry(kWavesPerSimdTarget), 1);

    // No counterpart on a host: 0 = unknown, per the geometry contract.
    for (int32_t k : {kMaxThreadsPerBlock, kLdsPerBlock, kLdsPerMp, kBlocksPerMp,
                      kL2, kRegsPerMp, kThreadsPerMp, kClockKHz})
        EXPECT_EQ(__cajeta_xpu_device_geometry(k), 0) << "key " << k;
}

// The shape that motivated the change must not come back: an RTX 4090 reads
// 128 x 4 x wave 32 with a 65536-register file. On the cpu backend none of
// those numbers can appear whatever card is installed.
TEST(XpuDeviceGeometryCpu, noGpuShapeLeaksIntoTheCpuBackend) {
    if (!forceCpuBackend()) GTEST_SKIP() << "could not select the cpu backend";
    EXPECT_NE(__cajeta_xpu_device_geometry(kWaveSize), 32);
    EXPECT_NE(__cajeta_xpu_device_geometry(kWaveSize), 64);
    EXPECT_NE(__cajeta_xpu_device_geometry(kSimdsPerMp), 4);
    EXPECT_NE(__cajeta_xpu_device_geometry(kSimdsPerMp), 8);
    EXPECT_EQ(__cajeta_xpu_device_geometry(kRegsPerMp), 0);
}
