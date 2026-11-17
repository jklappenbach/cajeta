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
#include <string>

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int32_t __cajeta_xpu_active_backend_id(void);
int64_t __cajeta_xpu_buffer_alloc(void* self, uint64_t byteCount, int32_t kind);
void __cajeta_xpu_buffer_free(void* self, int64_t handle, int32_t kind);
void __cajeta_xpu_cpu_set_hugepages(int32_t on);
void __cajeta_xpu_cpu_set_mmap_min(int64_t bytes);
int64_t __cajeta_xpu_buffer_cache_bytes(void);
void __cajeta_xpu_buffer_cache_flush(void);
void __cajeta_xpu_buffer_cache_set_limit(int64_t bytes);
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

// Resident kB of this process, from /proc/self/statm; -1 when unreadable.
long rssKb() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, res = 0;
    int got = std::fscanf(f, "%ld %ld", &size, &res);
    std::fclose(f);
    return got == 2 ? res * 4 : -1;
}

// The name of the /proc/self/maps mapping holding `addr` ("" for an anonymous one), or "?" when none holds it.
std::string mappingOf(uint64_t addr) {
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return "?";
    char line[512];
    std::string got = "?";
    while (std::fgets(line, sizeof line, f)) {
        unsigned long lo = 0, hi = 0;
        int off = 0;
        if (std::sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &lo, &hi, &off) >= 2 && addr >= lo && addr < hi) {
            std::string rest(line + off);
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == ' ')) rest.pop_back();
            got = rest;
            break;
        }
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

// xpu-buffer-host-memory 2.2: with the cache off, freed buffers of 1 MB or more give their pages back, so RSS
// follows the buffers alive. Eight written 8 MB buffers, the bottom seven freed: glibc's heap keeps them resident under the top one
// once a freed 16 MB mapping has raised its mmap threshold (it rises only for frees of 32 MB or less).
TEST(XpuCpuBufferAlign, freedLargeBuffersGiveTheirPagesBack) {
#if !defined(__linux__)
    GTEST_SKIP() << "measured through /proc";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    __cajeta_xpu_cpu_set_hugepages(0);
    __cajeta_xpu_buffer_cache_set_limit(0);
    void* big = std::malloc(16u << 20);
    std::memset(big, 1, 4096);
    std::free(big);
    const uint64_t n = 8u << 20;
    int64_t h[8];
    for (int i = 0; i < 8; i++) {
        h[i] = __cajeta_xpu_buffer_alloc(nullptr, n, kKindDevice);
        ASSERT_NE(h[i], 0);
        EXPECT_EQ(h[i] % 64, 0);
        std::memset((void*) (intptr_t) h[i], 1, n);
    }
    long before = rssKb();
    for (int i = 0; i < 7; i++) __cajeta_xpu_buffer_free(nullptr, h[i], kKindDevice);
    long after = rssKb();
    __cajeta_xpu_buffer_free(nullptr, h[7], kKindDevice);
    __cajeta_xpu_buffer_cache_set_limit(-2);
    __cajeta_xpu_cpu_set_hugepages(1);
    ASSERT_GT(before, 0);
    EXPECT_GE(before - after, 50L * 1024) << "rss " << before << " kB to " << after << " kB";
#endif
}

// xpu-buffer-host-memory 2.2: a buffer under the threshold still comes from malloc's heap.
TEST(XpuCpuBufferAlign, aSmallBufferStillComesFromMalloc) {
#if !defined(__linux__)
    GTEST_SKIP() << "read through /proc";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    int64_t h = __cajeta_xpu_buffer_alloc(nullptr, 64u << 10, kKindDevice);
    ASSERT_NE(h, 0);
    EXPECT_EQ(h % 64, 0);
    std::memset((void*) (intptr_t) h, 1, 64u << 10);
    EXPECT_EQ(mappingOf((uint64_t) h), "[heap]");
    __cajeta_xpu_buffer_free(nullptr, h, kKindDevice);
#endif
}

// xpu-buffer-host-memory 2.3: a freed mapped block serves the next allocation of its size, so a loop does not fault
// its pages in again.
TEST(XpuCpuBufferAlign, aFreedLargeBufferServesTheNextOfItsSize) {
#if !defined(__linux__)
    GTEST_SKIP() << "mapped buffers are Linux's";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    __cajeta_xpu_buffer_cache_flush();
    int64_t a = __cajeta_xpu_buffer_alloc(nullptr, 4u << 20, kKindDevice);
    ASSERT_NE(a, 0);
    __cajeta_xpu_buffer_free(nullptr, a, kKindDevice);
    int64_t b = __cajeta_xpu_buffer_alloc(nullptr, 4u << 20, kKindDevice);
    EXPECT_EQ(a, b);
    __cajeta_xpu_buffer_free(nullptr, b, kKindDevice);
    __cajeta_xpu_buffer_cache_flush();
#endif
}

// xpu-buffer-host-memory 2.3: the cpu cache keeps at most 64 MB, so a burst of frees past it goes back to the OS.
TEST(XpuCpuBufferAlign, theCpuCacheKeepsAtMostItsCap) {
#if !defined(__linux__)
    GTEST_SKIP() << "measured through /proc";
#else
    if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
    __cajeta_xpu_cpu_set_hugepages(0);
    __cajeta_xpu_buffer_cache_flush();
    const uint64_t n = 8u << 20;
    int64_t h[16];
    for (int i = 0; i < 16; i++) {
        h[i] = __cajeta_xpu_buffer_alloc(nullptr, n, kKindDevice);
        ASSERT_NE(h[i], 0);
        std::memset((void*) (intptr_t) h[i], 1, n);
    }
    long before = rssKb();
    for (int i = 0; i < 16; i++) __cajeta_xpu_buffer_free(nullptr, h[i], kKindDevice);
    long after = rssKb();
    int64_t kept = __cajeta_xpu_buffer_cache_bytes();
    __cajeta_xpu_buffer_cache_flush();
    __cajeta_xpu_cpu_set_hugepages(1);
    EXPECT_LE(kept, 64LL << 20);
    EXPECT_GE(before - after, 60L * 1024) << "rss " << before << " kB to " << after << " kB, cache " << kept;
#endif
}
