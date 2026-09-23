//
// The persistent CPU-kernel worker pool (g_caj_kpool, cajeta_xpu_dispatch.c)
// must uphold ONE contract: when a launch returns, every block of that launch
// has run. `caj_kpool_join` used to wait only on `active`, which counted the
// RUNNERS of a dispatch (the workers with myid < njobs). The pool's other
// workers (myid >= njobs, i.e. a launch whose grid needs fewer workers than
// the pool holds) still woke on every generation and read the shared, mutating
// `njobs` / `slices` / `active` — state the join did not wait for. A worker
// that woke for a SMALL launch (njobs small) but was slow to make its
// `myid < njobs` decision could read a later BIG launch's njobs, run that
// launch's slice, and decrement its `active` a second time. The big launch's
// join then returned early, while real workers were still executing its
// blocks against an argv that lived on the launcher's stack frame — which the
// caller, believing the launch complete, promptly reused. The stragglers
// dereferenced freed memory: a nondeterministic null-argv SIGSEGV that aborted
// the whole cpu llm suite (deqMw8FingerprintsAcrossFormats, q80WidenKernel).
//
// This exercises the pool through its real native entry points, alternating a
// small (few-block) launch and a big (many-block) launch — the interleave that
// produced the straggler — and after each launch asserts the contract: the
// per-block completion count equals the block count exactly. A straggler makes
// it short (worker still running) or long (a prior launch's straggler landing
// late); either fails the count. The kernel spins briefly so a mis-joined
// straggler is unambiguously still in flight when the count is read.
//
// AOT/native: the test drives cajeta_runtime.c linked into the test binary, so
// it needs no JIT and no GPU. It bundles + forces the CPU backend itself.

#include "gtest/gtest.h"

#include <atomic>
#include <cstdint>
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
}

namespace {

// CAJ_XPU_CPU ordinal (xpu/core dispatch enum).
constexpr int32_t kBackendCpu = 3;

// What one probe launch hands its per-block thunk. The struct lives on the
// launcher's stack (as a real kernel's argv slots do); `done` and `out` point
// at stable heap the harness owns, so a straggler reading a live struct still
// records against real memory rather than only crashing.
struct ProbeCtx {
    std::atomic<int>* done;   // per-block completion tally (heap, stable)
    int32_t* out;             // per-block magic sink (heap, stable), size nblocks
    int32_t magic;            // this launch's sentinel
    int32_t nblocks;          // grid X (1-D)
    int32_t spin;             // busy-work iterations, widening the straggler window
};

// Registered as a per-BLOCK CPU launch thunk: signature (void** argv, coord).
// coord[3] is ctaid.x (the 1-D block index). One tally increment per block.
extern "C" void cajeta_pool_probe_thunk(void** argv, const int32_t* coord) {
    ProbeCtx* c = reinterpret_cast<ProbeCtx*>(argv);
    int32_t b = coord[3];
    volatile int32_t sink = 0;
    for (int32_t i = 0; i < c->spin; ++i) sink += i;
    if (b >= 0 && b < c->nblocks) c->out[b] = c->magic;
    c->done->fetch_add(1, std::memory_order_relaxed);
    (void) sink;
}

bool forceCpuBackend() {
    // Bundle + force BEFORE any active-backend query: the first query caches the
    // selection, and querying with nothing bundled would latch CAJ_XPU_NONE.
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

// Run one probe launch and assert the completion contract holds the instant it
// returns: exactly `gx` blocks ran, and every sink slot carries this launch's
// magic. `ctx` is deliberately a caller-stack local so consecutive launches
// reuse the same frame — the aliasing that turned a straggler into a fault.
void launchAndAssertComplete(std::atomic<int>& done, std::vector<int32_t>& out,
                             int32_t gx, int32_t bx, int32_t magic, int32_t spin,
                             int iter, const char* tag) {
    ProbeCtx ctx{&done, out.data(), magic, gx, spin};
    done.store(0, std::memory_order_relaxed);
    __cajeta_xpu_launch_v3("cajeta.pool.probe", gx, 1, 1, bx, 1, 1,
                           /*sharedBytes=*/0, &ctx, /*stream=*/0,
                           /*deviceId=*/-1, /*specCount=*/0, /*specValues=*/nullptr);
    int ran = done.load(std::memory_order_acquire);
    ASSERT_EQ(ran, gx) << tag << " launch returned with " << ran << " of " << gx
                       << " blocks run (iter " << iter
                       << "): the pool join did not wait for every worker";
    for (int32_t b = 0; b < gx; ++b) {
        ASSERT_EQ(out[b], magic) << tag << " block " << b
            << " sink not written by this launch (iter " << iter << ")";
    }
}

} // namespace

// A big launch's join must not return while stragglers still run, even when a
// small launch precedes it on the same thread. Before the fix this reproduced
// the deqMw8Fingerprint SIGSEGV as an early return (short completion count) or
// a fault on the reused stack; after it, every launch closes cleanly.
TEST(XpuCpuPoolJoin, aLaunchReturnsOnlyAfterEveryBlockHasRun) {
    if (!forceCpuBackend()) {
        GTEST_SKIP() << "cpu backend not selectable in this process";
    }
    __cajeta_xpu_register_cpu_kernel("cajeta.pool.probe",
                                     reinterpret_cast<void*>(&cajeta_pool_probe_thunk));

    // The pool only fans a launch out when the grid clears the parallel
    // threshold; a single-core box runs everything inline and cannot straggle.
    // The test still asserts the contract there — it simply cannot fail.
    const int32_t kBigGrid = 64;     // many blocks -> most/all pool workers
    const int32_t kSmallGrid = 2;    // few blocks  -> njobs small, most idle
    const int32_t kBlock = 64;       // block * grid clears the 256-work-item floor
    const int kIters = 4000;

    std::atomic<int> done{0};
    std::vector<int32_t> outBig((size_t) kBigGrid, -1);
    std::vector<int32_t> outSmall((size_t) kSmallGrid, -1);

    for (int iter = 0; iter < kIters; ++iter) {
        // Small then big: the interleave that let a small-launch straggler
        // land on the big launch's active counter. Stop at the first breach —
        // the fatal failure is the result; running on only risks a straggler
        // faulting on freed memory and masking it as a crash.
        launchAndAssertComplete(done, outSmall, kSmallGrid, 128,
                                0x5A000000 | iter, /*spin=*/400, iter, "small");
        if (::testing::Test::HasFatalFailure()) return;
        launchAndAssertComplete(done, outBig, kBigGrid, kBlock,
                                0x0B000000 | iter, /*spin=*/1500, iter, "big");
        if (::testing::Test::HasFatalFailure()) return;
    }
}
