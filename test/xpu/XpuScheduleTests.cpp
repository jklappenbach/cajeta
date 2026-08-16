//
// CajetaXPU — Schedule (xpu-kernel-scheduling-hints Unit 1): the portable
// instruction-scheduling-hint surface (cajeta.xpu.Schedule).
//
// Schedule.barrier/groupBarrier/priority/pipelineOpt are device-only intrinsics
// (empty-bodied, call-site lowered like AsyncCopy). They are OPTIMIZATION
// DIRECTIVES — on a backend with no native scheduling intrinsic (CPU here) all
// four lower to NO-OPs, so a kernel that uses them must produce byte-identical
// output to the same kernel without them (spec §1.3.2 correctness-preserving).
// AMD lowers them to sched_barrier/sched_group_barrier/s_setprio/iglp_opt (U2).
//
// Two layers, both GPU-free (CPU oracle):
//  - scheduleControlsRunNoOpOnCpu: JIT-run an A/B (hinted vs plain) — identical.
//  - operand validation (spec §2.2): drive kernel lowering directly (the registration
//    path swallows lowering exceptions, so launch+compile would hide the diagnostic);
//    a non-constant or out-of-range ImmArg operand must throw cajeta::Exception.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto baseDir = std::filesystem::temp_directory_path()
              / ("cajeta_sched_" + std::to_string(rng()));
    std::filesystem::create_directories(baseDir / "test");
    std::ofstream(baseDir / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_sched_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = baseDir / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), baseDir.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// Lower kernel `entry` in `M` to a fresh CPU host module — the GPU-free path that
// PROPAGATES lowering diagnostics (unlike the registration path, which swallows
// them). Throws cajeta::Exception on a lowering error.
void lowerKernelToCpu(Compiler& compiler, const char* src, const char* entry) {
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], entry);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("sched_lower", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    cajeta::xpu::cpu::lowerKernel(k, *host);
}

// Two kernels computing the SAME value over a shared tile — one plain, one with
// all four Schedule.* controls placed through the body (a group-barrier WMMA/DS-
// style interleave point, a priority bump, a pipeline-opt hint at entry, a plain
// scheduling fence). On CPU the hints are no-ops, so viaHinted[i] == viaPlain[i]
// for every lane proves the surface is correctness-preserving. (mask 0x0020 =
// the VALU class; 0x0100 = the DS class — real sched_barrier mask bits.)
const char* kHintedVsPlainSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.Schedule;
public class M {
    @Kernel
    public static void viaHinted(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Schedule.pipelineOpt(0);
        Shared<int32> tile = shared int32[256];
        uint32 t = KernelThread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        Schedule.priority(3);
        Schedule.barrier(0);
        int32 acc = tile[t] + tile[(t + 7) & 255];
        Schedule.groupBarrier(32, 4, 0);
        acc = acc + tile[(t + 19) & 255];
        Schedule.priority(0);
        out[t] = acc;
    }
    @Kernel
    public static void viaPlain(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> tile = shared int32[256];
        uint32 t = KernelThread.x();
        tile[t] = in[t];
        Barrier.workgroup();
        int32 acc = tile[t] + tile[(t + 7) & 255];
        acc = acc + tile[(t + 19) & 255];
        out[t] = acc;
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hin = heap int32[n];
        int32[] hA = heap int32[n];
        int32[] hB = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) (i * 5 + 2); hA[i] = 0; hB[i] = 0; }
        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> oa = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> ob = heap KernelBuffer<int32>(n);
        in.upload(hin);
        oa.upload(hA);
        ob.upload(hB);
        KernelStream s #= KernelStream.current();
        viaHinted.launch(s, grid: [1], block: [256])(oa, in);
        viaPlain.launch(s, grid: [1], block: [256])(ob, in);
        s.sync();
        oa.download(hA);
        ob.download(hB);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hA[i] != hB[i]) { return (int32) (100 + i); }
        }
        return 555;
    }
}
)CJ";

// A non-constant mask (a runtime value) for Schedule.barrier — the operand is an
// ImmArg, so lowering must throw a clean diagnostic, not an LLVM crash.
const char* kNonConstMaskSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Schedule;
public class M {
    @Kernel
    public static void k(KernelBuffer<int32> out) {
        uint32 t = KernelThread.x();
        Schedule.barrier(t);
        out[t] = (int32) t;
    }
}
)CJ";

// Priority must be 0..3 (s_setprio is a 2-bit field). 7 is out of range → throw.
const char* kOutOfRangePrioSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Schedule;
public class M {
    @Kernel
    public static void k(KernelBuffer<int32> out) {
        uint32 t = KernelThread.x();
        Schedule.priority(7);
        out[t] = (int32) t;
    }
}
)CJ";

} // namespace

// U1.1.a + U1.3: a kernel using all four Schedule.* controls compiles, registers,
// and runs byte-identical to the same kernel without them on --xpu-backend=cpu
// (the no-op contract, spec §1.3.2).
TEST(XpuScheduleTests, scheduleControlsRunNoOpOnCpu) {
    auto jit = CajetaJit::compile(kHintedVsPlainSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 555) << "fail code " << r << " (100+i: hinted[i] != plain[i])";
}

// U1.1.b: a non-constant ImmArg operand is a clean lowering diagnostic, not a crash.
TEST(XpuScheduleTests, scheduleRejectsNonConstOperand) {
    Compiler compiler;
    EXPECT_THROW(lowerKernelToCpu(compiler, kNonConstMaskSource, "k"),
                 cajeta::Exception);
}

// U1.1.c: an out-of-range priority (not 0..3) is a clean lowering diagnostic.
TEST(XpuScheduleTests, scheduleRejectsOutOfRangeOperand) {
    Compiler compiler;
    EXPECT_THROW(lowerKernelToCpu(compiler, kOutOfRangePrioSource, "k"),
                 cajeta::Exception);
}
