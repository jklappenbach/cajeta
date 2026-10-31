//
// Promotion by measured work (cpu-isa-variants 5.3.3.3). The cpu pool runs a
// launch under its work-item floor inline, which judges the launch by its
// work-item count; a kernel whose work-item is a whole 4096-column dot over a
// row group ran single-threaded on the caller while the pool waited. The
// runtime now times an inline launch, and a kernel that measures over the
// promote threshold twice fans out from then on. These tests drive the pool
// through its native entry points and read the launch trace to see which path
// each launch took.
//

#include "gtest/gtest.h"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int32_t __cajeta_xpu_active_backend_id(void);
void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn);
void __cajeta_xpu_launch_v3(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId,
                            int32_t specCount, const int32_t* specValues);
int32_t __cajeta_xpu_cpu_pool_threads(void);

// Mirrors struct cajeta_cpu_trace_row in cajeta_xpu_dispatch.c.
struct cajeta_cpu_trace_row {
    const char* name;
    int64_t launches, serial, wallNs, criticalNs, idleNs, lagNs, gapNs, callerLateNs, serialNs, workers;
};
void __cajeta_xpu_cpu_trace_set(int32_t on);
void __cajeta_xpu_cpu_trace_reset(void);
int32_t __cajeta_xpu_cpu_trace_rows(void);
int32_t __cajeta_xpu_cpu_trace_row(int32_t i, struct cajeta_cpu_trace_row* out);

void __cajeta_xpu_cpu_set_promote_ns(int64_t ns);
int64_t __cajeta_xpu_cpu_promote_ns(void);
int32_t __cajeta_xpu_cpu_promoted_count(void);
void __cajeta_xpu_cpu_promote_reset(void);
}

namespace {

constexpr int32_t kBackendCpu = 3;

struct ProbeCtx {
    std::atomic<int>* done;
    int32_t spin;
};

extern "C" void cajeta_promote_probe_thunk(void** argv, const int32_t* coord) {
    ProbeCtx* c = reinterpret_cast<ProbeCtx*>(argv);
    volatile int32_t sink = 0;
    for (int32_t i = 0; i < c->spin; ++i) sink += i;
    c->done->fetch_add(1, std::memory_order_relaxed);
    (void) sink;
    (void) coord;
}

bool forceCpuBackend() {
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

// A launch of `gx` blocks of `bx` work-items; gx * bx stays under the 256 floor.
void launch(const char* name, int32_t gx, int32_t bx, int32_t spin) {
    std::atomic<int> done{0};
    ProbeCtx ctx{&done, spin};
    __cajeta_xpu_launch_v3(name, gx, 1, 1, bx, 1, 1, 0, &ctx, 0, -1, 0, nullptr);
    ASSERT_EQ(done.load(), gx);
}

cajeta_cpu_trace_row rowNamed(const char* name) {
    int32_t n = __cajeta_xpu_cpu_trace_rows();
    for (int32_t i = 0; i < n; ++i) {
        cajeta_cpu_trace_row r{};
        if (__cajeta_xpu_cpu_trace_row(i, &r) && r.name && std::strcmp(r.name, name) == 0) return r;
    }
    return cajeta_cpu_trace_row{};
}

struct XpuCpuPromote : ::testing::Test {
    void SetUp() override {
        if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
        const char* names[] = {"cajeta.promote.heavy", "cajeta.promote.light",
                               "cajeta.promote.knob", "cajeta.promote.oneblock"};
        for (const char* n : names)
            __cajeta_xpu_register_cpu_kernel(n, reinterpret_cast<void*>(&cajeta_promote_probe_thunk));
        // The pool is made by the first parallel launch; warm it before asking its size.
        __cajeta_xpu_cpu_trace_set(0);
        launch("cajeta.promote.heavy", 64, 64, 10);
        if (__cajeta_xpu_cpu_pool_threads() < 1) GTEST_SKIP() << "no pool workers on this box";
        __cajeta_xpu_cpu_promote_reset();
        __cajeta_xpu_cpu_set_promote_ns(0);
        __cajeta_xpu_cpu_trace_set(1);
        __cajeta_xpu_cpu_trace_reset();
    }
    void TearDown() override {
        __cajeta_xpu_cpu_trace_set(0);
        __cajeta_xpu_cpu_trace_reset();
        __cajeta_xpu_cpu_promote_reset();
        __cajeta_xpu_cpu_set_promote_ns(0);
    }
};

} // namespace

// FIRES: four blocks of heavy work (well over 20 us inline) under the floor run
// inline twice, then fan out: the third launch is parallel.
TEST_F(XpuCpuPromote, aKernelThatMeasuresLongTwiceInlineFansOut) {
    EXPECT_EQ(__cajeta_xpu_cpu_promote_ns(), 20000);
    for (int i = 0; i < 4; ++i) launch("cajeta.promote.heavy", 4, 16, 200000);
    cajeta_cpu_trace_row r = rowNamed("cajeta.promote.heavy");
    EXPECT_EQ(r.serial, 2);
    EXPECT_EQ(r.launches, 2);
    EXPECT_EQ(__cajeta_xpu_cpu_promoted_count(), 1);
}

// DOES NOT FIRE: a light kernel under the floor stays inline however often it runs.
TEST_F(XpuCpuPromote, aLightKernelStaysInline) {
    for (int i = 0; i < 6; ++i) launch("cajeta.promote.light", 4, 16, 1);
    cajeta_cpu_trace_row r = rowNamed("cajeta.promote.light");
    EXPECT_EQ(r.serial, 6);
    EXPECT_EQ(r.launches, 0);
    EXPECT_EQ(__cajeta_xpu_cpu_promoted_count(), 0);
}

// The threshold is a knob: at 1 ns the light kernel promotes after two launches;
// 0 restores the default.
TEST_F(XpuCpuPromote, theThresholdIsAKnob) {
    __cajeta_xpu_cpu_set_promote_ns(1);
    EXPECT_EQ(__cajeta_xpu_cpu_promote_ns(), 1);
    for (int i = 0; i < 3; ++i) launch("cajeta.promote.knob", 4, 16, 1);
    cajeta_cpu_trace_row r = rowNamed("cajeta.promote.knob");
    EXPECT_EQ(r.serial, 2);
    EXPECT_EQ(r.launches, 1);
    __cajeta_xpu_cpu_set_promote_ns(0);
    EXPECT_EQ(__cajeta_xpu_cpu_promote_ns(), 20000);
}

// A one-block launch has nothing to fan out, so it is never promoted, however heavy.
TEST_F(XpuCpuPromote, aOneBlockLaunchIsNeverPromoted) {
    for (int i = 0; i < 4; ++i) launch("cajeta.promote.oneblock", 1, 64, 200000);
    cajeta_cpu_trace_row r = rowNamed("cajeta.promote.oneblock");
    EXPECT_EQ(r.serial, 4);
    EXPECT_EQ(r.launches, 0);
    EXPECT_EQ(__cajeta_xpu_cpu_promoted_count(), 0);
}
