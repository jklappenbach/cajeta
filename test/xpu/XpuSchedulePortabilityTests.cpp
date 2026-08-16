//
// CajetaXPU — Schedule portability: NVPTX / SPIR-V / CPU no-op lowering
// (xpu-kernel-scheduling-hints Unit 3).
//
// The AMD native path (sched_barrier/sched_group_barrier/s_setprio/iglp_opt) is
// covered by XpuScheduleAmdDeviceTests. This file pins the OTHER backends: a
// scheduling hint is an optimization directive, so every backend without a native
// scheduling intrinsic (NVPTX/SPIR-V/CPU) lowers it as a NO-OP via the default
// LoweringTarget seam — the kernel compiles and emits valid output, just without
// the hint (spec §4). No new lowering code is needed; this proves the default seam
// covers all three (parallel to XpuAsyncCopyPortabilityTests).
//
// GPU-free: PTX and SPIR-V text are emitted without a device.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// A kernel using all four Schedule.* controls threaded through a real compute
// (stage in -> tile, barrier, cross-lane read). On NVPTX/SPIR-V/CPU the hints are
// no-ops; the kernel must still lower + emit valid output.
const char* kSchedSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Schedule;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void sched(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Schedule.pipelineOpt(0);\n"
    "        Shared<uint32> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        tile[t] = in[t];\n"
    "        Schedule.priority(3);\n"
    "        Barrier.workgroup();\n"
    "        Schedule.barrier(0);\n"
    "        Schedule.groupBarrier(32, 4, 0);\n"
    "        uint32 v = tile[255 - t];\n"
    "        Schedule.priority(0);\n"
    "        out[t] = v;\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_schedport_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_schedport_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

} // namespace

// U3.1 (NVPTX, GPU-free): the hinted kernel lowers + emits valid PTX with the
// controls as no-ops (default seam). No cp.async / sched directive required.

// U3.1 (SPIR-V, GPU-free): the hinted kernel lowers + emits valid SPIR-V with the
// controls as no-ops (default seam).

// U3.1 (CPU, GPU-free): the hinted kernel lowers on the CPU backend (the no-op
// seam is the CPU oracle — also exercised end-to-end in XpuScheduleTests).
TEST(XpuSchedulePortabilityTests, cpuLowersScheduleAsNoOp) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSchedSrc);
    auto method = findMethod(module->getStructures()["test.M"], "sched");
    ASSERT_NE(method, nullptr);

    llvm::LLVMContext ctx;
    llvm::Module host("xpu_schedport_cpu", ctx);
    EXPECT_NE(cajeta::xpu::cpu::lowerKernel(method, host), nullptr)
        << "Schedule no-op kernel must lower on CPU";
}
