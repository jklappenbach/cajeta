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
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int32_t __cajeta_xpu_active_backend_id(void);
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount, int32_t kind);
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind);
void __cajeta_xpu_cpu_set_hugepages(int32_t on);
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

// The THPeligible line of the /proc/self/smaps mapping holding `addr`, or -1 when none is found.
int thpEligible(uint64_t addr) {
    FILE* f = std::fopen("/proc/self/smaps", "r");
    if (!f) return -1;
    char line[512];
    bool inside = false;
    int got = -1;
    while (std::fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(line, "%lx-%lx ", &lo, &hi) == 2) {
            inside = addr >= lo && addr < hi;
            continue;
        }
        int v = 0;
        if (inside && std::sscanf(line, "THPeligible: %d", &v) == 1) { got = v; break; }
    }
    std::fclose(f);
    return got;
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

// cajeta-llm 15.2.20.3.16.15.3: a buffer of 4 MiB or more (the first size sure to hold an aligned 2 MiB extent)
// keeps its 64-byte start and its pages are eligible for transparent huge pages.
TEST(XpuCpuBufferAlign, aLargeBufferIsHugePageEligible) {
#if !defined(__linux__)
    GTEST_SKIP() << "transparent huge pages are Linux's";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    __cajeta_xpu_cpu_set_hugepages(1);
    const uint64_t sizes[] = {4u << 20, (1u << 23) + 2432, 48u << 20};
    for (uint64_t n : sizes) {
        int64_t h = __cajeta_xpu_buffer_alloc(nullptr, n, kKindDevice);
        ASSERT_NE(h, 0);
        EXPECT_EQ(h % 64, 0) << n << " bytes: handle " << std::hex << h;
        EXPECT_EQ(thpEligible((uint64_t) h + n / 2), 1) << n << " bytes";
        __cajeta_xpu_buffer_free(nullptr, h, kKindDevice);
    }
#endif
}

// With huge pages off, and below 2 MiB, the buffer keeps the 64-byte rule and asks for nothing.
TEST(XpuCpuBufferAlign, hugePagesOffOrSmallAskNothing) {
#if !defined(__linux__)
    GTEST_SKIP() << "transparent huge pages are Linux's";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    FILE* m = std::fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    char mode[128] = {0};
    if (m) { std::fgets(mode, sizeof mode, m); std::fclose(m); }
    if (!std::strstr(mode, "[madvise]")) GTEST_SKIP() << "THP is not in madvise mode: " << mode;
    __cajeta_xpu_cpu_set_hugepages(0);
    int64_t h = __cajeta_xpu_buffer_alloc(nullptr, 48u << 20, kKindDevice);
    ASSERT_NE(h, 0);
    EXPECT_EQ(h % 64, 0);
    EXPECT_EQ(thpEligible((uint64_t) h + (24u << 20)), 0) << "huge pages off";
    __cajeta_xpu_buffer_free(nullptr, h, kKindDevice);
    __cajeta_xpu_cpu_set_hugepages(1);
#endif
}

// After the process frees a large block, glibc serves 2 MiB to 32 MiB from its heap instead of a fresh
// mapping (its dynamic mmap threshold), as in a long-lived test shard; the advice still holds there.
TEST(XpuCpuBufferAlign, aLargeBufferIsHugePageEligibleAfterTheMmapThresholdRises) {
#if !defined(__linux__)
    GTEST_SKIP() << "transparent huge pages are Linux's";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    __cajeta_xpu_cpu_set_hugepages(1);
    void* big = std::malloc(40u << 20);
    std::memset(big, 1, 4096);
    std::free(big);
    const uint64_t sizes[] = {4u << 20, (1u << 23) + 2432, 24u << 20};
    for (uint64_t n : sizes) {
        int64_t h = __cajeta_xpu_buffer_alloc(nullptr, n, kKindDevice);
        ASSERT_NE(h, 0);
        EXPECT_EQ(thpEligible((uint64_t) h + n / 2), 1) << n << " bytes at " << std::hex << h;
        __cajeta_xpu_buffer_free(nullptr, h, kKindDevice);
    }
#endif
}
