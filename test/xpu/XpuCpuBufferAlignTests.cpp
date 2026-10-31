//
// A cpu-backend device buffer is 64-byte aligned (cpu-isa-variants 5.3.3). The
// cpu backend's "device" memory came from plain malloc, which hands a large
// block back 16 bytes past a page boundary; every 512-bit load in a 16-row
// kernel then straddled two cache lines, and half of an 8-row kernel's 256-bit
// loads did. These tests allocate through the runtime's buffer entry point
// with the cpu backend forced and read the handle's alignment.
//

#include "gtest/gtest.h"

#include <cstdint>

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int32_t __cajeta_xpu_active_backend_id(void);
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount, int32_t kind);
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind);
}

namespace {

constexpr int32_t kBackendCpu = 3;
constexpr int32_t kKindDevice = 0;
constexpr int32_t kKindPinned = 1;
constexpr int32_t kKindUnified = 2;

bool forceCpuBackend() {
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

} // namespace

// Every size class a kernel sees, small through mmap-sized, lands on a 64-byte
// boundary, for every memory kind the cpu backend serves from the host.
TEST(XpuCpuBufferAlign, everyBufferIsSixtyFourByteAligned) {
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    const uint64_t sizes[] = {1, 64, 100, 4096, 4097, 1u << 20, (1u << 23) + 2432};
    const int32_t kinds[] = {kKindDevice, kKindPinned, kKindUnified};
    for (int32_t kind : kinds) {
        for (uint64_t n : sizes) {
            int64_t h = __cajeta_xpu_buffer_alloc(nullptr, n, kind);
            ASSERT_NE(h, 0) << "alloc of " << n << " bytes, kind " << kind;
            EXPECT_EQ(h % 64, 0) << n << " bytes, kind " << kind << ": handle " << std::hex << h;
            __cajeta_xpu_buffer_free(nullptr, h, kind);
        }
    }
}

// Freed and reallocated in a churn, the alignment holds (the allocator's reuse
// path is the same one a model's weights come through).
TEST(XpuCpuBufferAlign, alignmentHoldsAcrossChurn) {
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    int64_t keep[8] = {0};
    for (int round = 0; round < 64; ++round) {
        uint64_t n = (uint64_t) (1216 * (1 + round % 7)) * 97;
        int64_t h = __cajeta_xpu_buffer_alloc(nullptr, n, kKindDevice);
        ASSERT_NE(h, 0);
        EXPECT_EQ(h % 64, 0) << "round " << round << ", " << n << " bytes";
        int slot = round % 8;
        if (keep[slot]) __cajeta_xpu_buffer_free(nullptr, keep[slot], kKindDevice);
        keep[slot] = h;
    }
    for (int64_t h : keep) if (h) __cajeta_xpu_buffer_free(nullptr, h, kKindDevice);
}
