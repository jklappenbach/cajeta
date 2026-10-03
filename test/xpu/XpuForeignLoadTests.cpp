//
// The foreign-load reading (xpu-tile-shape-selection plan 4.6.1.7, spec
// §7.12): ShapeChoice refuses to cache a measurement taken while another
// process uses the device, and this is the reading it asks.
//
// Measured 2026-10-03 on Phoenix (RTX 4090 under WSL2): NVML's compute-process
// list is EMPTY even with a CUDA spinner running in another process, while
// its utilization reads 87% against 0% idle. So the reading takes a foreign
// process from the list where the platform reports one, and utilization
// otherwise. Utilization is the whole device's, so the reading is the
// minimum of three samples 100 ms apart: this process's own finished work
// decays out of NVML's window, a foreign process that is still running does
// not.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

using cajeta_test::CajetaJit;

extern "C" int32_t __cajeta_xpu_foreign_load(void);

namespace {

const char* kSpin = R"CJ(
package test;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void burn(KernelBuffer<float32> out, uint32 iters) {
        uint32 i = KernelThread.globalIdX();
        float32 x = (float32) i;
        uint32 k = 0;
        while (k < iters) { x = x * 1.0000001f + 0.5f; k = k + 1; }
        out[i] = x;
    }
    public static int32 run() {
        KernelBuffer<float32> b #= heap KernelBuffer<float32>((uint64) 262144);
        KernelStream s #= KernelStream.current();
        int64 t0 = System.currentTimeMillis();
        while (System.currentTimeMillis() - t0 < 10000L) {
            burn.launch(s, grid: [1024], block: [256])(b, 20000);
            s.sync();
        }
        return 0;
    }
}
)CJ";

} // namespace

// Run only as the child of the test below: keeps the GPU busy for 10 s.
TEST(XpuForeignLoad, DISABLED_spinTheGpuForTenSeconds) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    auto jit = CajetaJit::compile(kSpin, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

TEST(XpuForeignLoad, anotherProcessOnTheGpuReadsAsForeignLoad) {
#if defined(_WIN32)
    GTEST_SKIP() << "the child-process fixture is POSIX";
#else
    CAJETA_SKIP_IF_NO_CUDA();
    const int32_t before = __cajeta_xpu_foreign_load();
    if (before < 0) GTEST_SKIP() << "NVML is not available on this box";
    if (before >= 25)
        GTEST_SKIP() << "the GPU is already " << before << "% busy (a CI runner?)";
    const pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        execl("/proc/self/exe", "cajeta_test", "--gtest_also_run_disabled_tests",
              "--gtest_filter=XpuForeignLoad.DISABLED_spinTheGpuForTenSeconds",
              (char*) nullptr);
        _exit(127);
    }
    int32_t seen = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(100);
    while (std::chrono::steady_clock::now() < until) {
        seen = std::max(seen, __cajeta_xpu_foreign_load());
        if (seen >= 25) break;
        int st = 0;
        if (waitpid(child, &st, WNOHANG) == child) break;   // spinner ended early
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    int status = 0;
    waitpid(child, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "the spinner child failed";
    EXPECT_GE(seen, 25) << "a foreign CUDA process was not seen";
#endif
}
