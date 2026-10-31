//
// The cpu pool's launch trace (cpu-isa-variants 5.3.3). A parallel cpu launch
// is a fork-join over static slices; when a decode runs at 0.93 of another
// form with the same bytes and fewer instructions, the question is where the
// wall time went: the slices themselves, workers waiting at the barrier for
// the slowest slice (idle), workers waking after a dispatch (lag), or the host
// between launches (gap). With the trace on, every parallel launch is stamped
// and summed per kernel name; these tests drive the pool through its native
// entry points and check that the sums say what they should.
//

#include "gtest/gtest.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

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
    int64_t launches;      // parallel launches
    int64_t serial;        // launches that ran inline on the caller
    int64_t wallNs;        // dispatch to join returned
    int64_t criticalNs;    // dispatch to the last slice's end
    int64_t idleNs;        // worker-ns: sum over slices of (last end - own end)
    int64_t lagNs;         // worker-ns: sum over slices of (own start - dispatch)
    int64_t gapNs;         // host-ns since the previous parallel launch's join
    int64_t callerLateNs;  // the caller's own slice past the last worker's end
    int64_t serialNs;
    int64_t workers;       // sum of nworkers over parallel launches
};
void __cajeta_xpu_cpu_trace_set(int32_t on);
void __cajeta_xpu_cpu_trace_reset(void);
int32_t __cajeta_xpu_cpu_trace_rows(void);
int32_t __cajeta_xpu_cpu_trace_row(int32_t i, struct cajeta_cpu_trace_row* out);
}

namespace {

constexpr int32_t kBackendCpu = 3;

struct ProbeCtx {
    std::atomic<int>* done;
    int32_t nblocks;
    int32_t spin;        // busy-work iterations for every block
    int32_t heavyBlock;  // this block spins 64x longer (-1 for none)
};

extern "C" void cajeta_trace_probe_thunk(void** argv, const int32_t* coord) {
    ProbeCtx* c = reinterpret_cast<ProbeCtx*>(argv);
    int32_t b = coord[3];
    int32_t n = c->spin * (b == c->heavyBlock ? 64 : 1);
    volatile int32_t sink = 0;
    for (int32_t i = 0; i < n; ++i) sink += i;
    c->done->fetch_add(1, std::memory_order_relaxed);
    (void) sink;
}

bool forceCpuBackend() {
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

void launch(const char* name, int32_t gx, int32_t bx, int32_t spin, int32_t heavy) {
    std::atomic<int> done{0};
    ProbeCtx ctx{&done, gx, spin, heavy};
    __cajeta_xpu_launch_v3(name, gx, 1, 1, bx, 1, 1, 0, &ctx, 0, -1, 0, nullptr);
    ASSERT_EQ(done.load(), gx);
}

bool rowNamed(const char* name, cajeta_cpu_trace_row* out) {
    int32_t n = __cajeta_xpu_cpu_trace_rows();
    for (int32_t i = 0; i < n; ++i) {
        cajeta_cpu_trace_row r{};
        if (__cajeta_xpu_cpu_trace_row(i, &r) && r.name && std::strcmp(r.name, name) == 0) {
            *out = r;
            return true;
        }
    }
    return false;
}

struct XpuCpuTrace : ::testing::Test {
    void SetUp() override {
        if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
        __cajeta_xpu_register_cpu_kernel("cajeta.trace.even",
                                         reinterpret_cast<void*>(&cajeta_trace_probe_thunk));
        __cajeta_xpu_register_cpu_kernel("cajeta.trace.uneven",
                                         reinterpret_cast<void*>(&cajeta_trace_probe_thunk));
        __cajeta_xpu_register_cpu_kernel("cajeta.trace.small",
                                         reinterpret_cast<void*>(&cajeta_trace_probe_thunk));
        // The pool is made by the first parallel launch; warm it before asking its size.
        __cajeta_xpu_cpu_trace_set(0);
        launch("cajeta.trace.even", 64, 64, 10, -1);
        if (__cajeta_xpu_cpu_pool_threads() < 1) GTEST_SKIP() << "no pool workers on this box";
        __cajeta_xpu_cpu_trace_set(1);
        __cajeta_xpu_cpu_trace_reset();
    }
    void TearDown() override {
        __cajeta_xpu_cpu_trace_set(0);
        __cajeta_xpu_cpu_trace_reset();
    }
};

} // namespace

// N even launches of one kernel are one row counting N, with the sums ordered
// as the stamps are: wall covers the critical path, which is positive, and the
// worker-ns sums are never negative. The gap sums N-1 host intervals.
TEST_F(XpuCpuTrace, evenLaunchesAreCountedWithOrderedSums) {
    const int kN = 8;
    for (int i = 0; i < kN; ++i) launch("cajeta.trace.even", 64, 64, 2000, -1);
    cajeta_cpu_trace_row r{};
    ASSERT_TRUE(rowNamed("cajeta.trace.even", &r));
    EXPECT_EQ(r.launches, kN);
    EXPECT_EQ(r.serial, 0);
    EXPECT_GT(r.criticalNs, 0);
    EXPECT_GE(r.wallNs, r.criticalNs);
    EXPECT_GE(r.idleNs, 0);
    EXPECT_GE(r.lagNs, 0);
    EXPECT_GE(r.gapNs, 0);
    EXPECT_GE(r.callerLateNs, 0);
    EXPECT_GE(r.workers, 2 * kN);
}

// FIRES: one block 64x heavier than the rest holds every other worker at the
// barrier, so the idle is most of the launch: at least (workers - 1) halves of
// the critical path summed over workers.
TEST_F(XpuCpuTrace, aHeavyBlockShowsAsIdleAtTheBarrier) {
    launch("cajeta.trace.uneven", 64, 64, 20000, 5);
    cajeta_cpu_trace_row r{};
    ASSERT_TRUE(rowNamed("cajeta.trace.uneven", &r));
    ASSERT_EQ(r.launches, 1);
    ASSERT_GT(r.workers, 1);
    EXPECT_GT(r.idleNs, (r.workers - 1) * (r.criticalNs / 2))
        << "idle " << r.idleNs << " ns, critical " << r.criticalNs << " ns, workers " << r.workers;
}

// A launch under the parallel floor runs inline and is counted as serial, with
// no parallel sums.
TEST_F(XpuCpuTrace, anInlineLaunchCountsAsSerial) {
    launch("cajeta.trace.small", 1, 32, 100, -1);
    cajeta_cpu_trace_row r{};
    ASSERT_TRUE(rowNamed("cajeta.trace.small", &r));
    EXPECT_EQ(r.launches, 0);
    EXPECT_EQ(r.serial, 1);
    EXPECT_GE(r.serialNs, 0);
    EXPECT_EQ(r.wallNs, 0);
}

// DOES NOT FIRE: with the trace off nothing is recorded.
TEST_F(XpuCpuTrace, offRecordsNothing) {
    __cajeta_xpu_cpu_trace_set(0);
    launch("cajeta.trace.even", 64, 64, 1000, -1);
    EXPECT_EQ(__cajeta_xpu_cpu_trace_rows(), 0);
}
