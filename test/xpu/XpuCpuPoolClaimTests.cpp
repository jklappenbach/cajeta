//
// The cpu kernel pool hands out a launch's blocks by claim, so a worker that is
// slow to wake (preempted at full occupancy, or held here by a test hook) does
// not hold the launch: the caller and the awake workers take its share, and the
// launch returns once every block has run (cajeta-llm 15.2.14.7).
//

#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
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
int32_t __cajeta_xpu_cpu_worker_cap(void);
void __cajeta_xpu_cpu_set_worker_cap(int32_t n);
int32_t __cajeta_xpu_cpu_last_workers(void);
void __cajeta_xpu_cpu_test_hold_worker(int32_t id, int64_t ns);
}

namespace {

constexpr int32_t kBackendCpu = 3;

struct ClaimCtx {
    std::atomic<int>* perBlock;   // one tally a block
    int32_t* magicOut;            // one sink a block
    int32_t magic;
    int32_t nblocks;
    int32_t spin;
};

extern "C" void cajeta_pool_claim_thunk(void** argv, const int32_t* coord) {
    ClaimCtx* c = reinterpret_cast<ClaimCtx*>(argv);
    int32_t b = coord[3];
    volatile int32_t sink = 0;
    for (int32_t i = 0; i < c->spin; ++i) sink += i;
    if (b >= 0 && b < c->nblocks) {
        c->magicOut[b] = c->magic;
        c->perBlock[b].fetch_add(1, std::memory_order_relaxed);
    }
    (void) sink;
}

bool forceCpuBackend() {
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    return __cajeta_xpu_active_backend_id() == kBackendCpu;
}

class XpuCpuPoolClaim : public ::testing::Test {
protected:
    int32_t savedCap = 0;
    void SetUp() override {
        if (!forceCpuBackend()) GTEST_SKIP() << "cpu backend not selectable in this process";
        __cajeta_xpu_register_cpu_kernel("cajeta.pool.claim",
                                         reinterpret_cast<void*>(&cajeta_pool_claim_thunk));
        savedCap = __cajeta_xpu_cpu_worker_cap();
        __cajeta_xpu_cpu_set_worker_cap(8);
        if (std::thread::hardware_concurrency() < 4) GTEST_SKIP() << "needs a pool of at least four";
        int64_t ms = 0;
        launch(64, 0x11, &ms);   // builds the pool before any hold
        if (__cajeta_xpu_cpu_pool_threads() < 3) GTEST_SKIP() << "the pool did not start workers";
    }
    void TearDown() override {
        __cajeta_xpu_cpu_test_hold_worker(-1, 0);
        __cajeta_xpu_cpu_set_worker_cap(savedCap);
    }

    // One launch of `gx` 64-wide blocks; asserts every block ran exactly once with this
    // launch's magic, and reports the launch's wall time.
    void launch(int32_t gx, int32_t magic, int64_t* ms, int32_t spin = 200) {
        std::unique_ptr<std::atomic<int>[]> per(new std::atomic<int>[(size_t) gx]);
        for (int32_t b = 0; b < gx; ++b) per[b].store(0, std::memory_order_relaxed);
        std::vector<int32_t> out((size_t) gx, -1);
        ClaimCtx ctx{per.get(), out.data(), magic, gx, spin};
        auto t0 = std::chrono::steady_clock::now();
        __cajeta_xpu_launch_v3("cajeta.pool.claim", gx, 1, 1, 64, 1, 1, 0, &ctx, 0, -1, 0, nullptr);
        *ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        for (int32_t b = 0; b < gx; ++b) {
            ASSERT_EQ(per[b].load(std::memory_order_acquire), 1) << "block " << b << " of " << gx;
            ASSERT_EQ(out[b], magic) << "block " << b << " of " << gx;
        }
    }
};

} // namespace

// FIRES on a static split: the held worker's slice waits out the hold.
TEST_F(XpuCpuPoolClaim, aHeldWorkerDoesNotHoldTheLaunch) {
    __cajeta_xpu_cpu_test_hold_worker(0, 400LL * 1000 * 1000);
    int64_t ms = 0;
    launch(64, 0x21, &ms);
    EXPECT_LT(ms, 150) << "the launch waited for the held worker";
}

// A worker that wakes after its launch finished runs none of it, and the launches
// that follow while it sleeps, and after it wakes, still run every block once.
TEST_F(XpuCpuPoolClaim, aLateWorkerRunsNothingOfAFinishedLaunch) {
    __cajeta_xpu_cpu_test_hold_worker(0, 150LL * 1000 * 1000);
    int64_t ms = 0;
    launch(64, 0x31, &ms);
    EXPECT_LT(ms, 100);
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    int32_t i = 0;
    while (std::chrono::steady_clock::now() < until) {
        launch(37 + (i % 50), 0x3100 + i, &ms, 50);
        if (::testing::Test::HasFatalFailure()) return;
        ++i;
    }
    EXPECT_GT(i, 10);
}

// FIRES on a static split: with every worker held the caller runs the launch alone.
TEST_F(XpuCpuPoolClaim, theCallerRunsTheLaunchAloneWhenEveryWorkerIsHeld) {
    __cajeta_xpu_cpu_test_hold_worker(-1, 400LL * 1000 * 1000);
    int64_t ms = 0;
    launch(64, 0x41, &ms);
    EXPECT_LT(ms, 150) << "the launch waited for held workers";
}

// Every block of an odd grid runs exactly once.
TEST_F(XpuCpuPoolClaim, everyBlockRunsExactlyOnce) {
    const int32_t grids[] = {4, 7, 31, 32, 33, 63, 64, 65, 127, 1000, 4099};
    int32_t magic = 0x5100;
    for (int32_t g : grids) {
        for (int r = 0; r < 20; ++r) {
            int64_t ms = 0;
            launch(g, magic++, &ms, 20);
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
}
