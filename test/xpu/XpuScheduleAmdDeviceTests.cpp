//
// CajetaXPU — Schedule native AMDGPU lowering (xpu-kernel-scheduling-hints Unit 2).
//
// Schedule.{barrier,groupBarrier,priority,pipelineOpt} lower to the amdgcn
// scheduling intrinsics: sched_barrier / sched_group_barrier / s_setprio /
// iglp_opt. Three of the four are SCHEDULER directives consumed by the
// MachineScheduler — they steer instruction order but emit no ISA instruction;
// only s_setprio is a real SOPP instruction that survives to the output. So the
// GPU-free checks are layered:
//   (1) IR (gfx1151) — every control emits its amdgcn intrinsic CALL (this is
//       what proves the lowering; the scheduler later consumes three of them);
//   (2) ISA (gfx1151) — the kernel compiles cleanly (no Cannot-select / LLVM
//       ERROR) and the one surviving mnemonic, s_setprio, is present;
//   (3) on-device (gfx1151, skips w/o HIP) — a hinted kernel is numerically
//       correct (scheduling hints never change results, spec §3.1.3).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Stage in[0..256) into an LDS tile, barrier, then read a DIFFERENT lane
// (tile[255-t]) — with all four Schedule.* hints threaded through the body. The
// hints are scheduling-only, so out[t] == in[255-t] == 255-t for every t must
// still hold. (mask 0 = all classes; groupBarrier(32,4,0) groups 4 VALU insts.)
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
              / ("cajeta_xpu_sched_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_sched_arch_" + std::to_string(rng()));
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

// Lower `sched` for `arch` and return its device IR (before ISA emission, so the
// scheduler hasn't yet consumed the directive intrinsics).
std::string emitIrForArch(const char* arch) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSchedSrc);
    auto method = findMethod(module->getStructures()["test.M"], "sched");
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine(arch);
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_sched_ir", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    std::string ir;
    llvm::raw_string_ostream os(ir);
    deviceModule.print(os, nullptr);
    os.flush();
    return ir;
}

std::string emitIsaForArch(const char* arch) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSchedSrc);
    auto method = findMethod(module->getStructures()["test.M"], "sched");
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine(arch);
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_sched_isa", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return emitIsa(deviceModule, *tm);
}

} // namespace

// U2.1.a (IR, GPU-free): each Schedule.* control emits its amdgcn intrinsic call.
TEST(XpuScheduleAmdDeviceTests, irEmitsSchedIntrinsics) {
    std::string ir = emitIrForArch("gfx1151");
    ASSERT_FALSE(ir.empty()) << "IR emit failed for gfx1151";
    EXPECT_NE(ir.find("@llvm.amdgcn.sched.barrier"), std::string::npos) << ir;
    EXPECT_NE(ir.find("@llvm.amdgcn.sched.group.barrier"), std::string::npos) << ir;
    EXPECT_NE(ir.find("@llvm.amdgcn.s.setprio"), std::string::npos) << ir;
    EXPECT_NE(ir.find("@llvm.amdgcn.iglp.opt"), std::string::npos) << ir;
}

// U2.1.b (ISA, GPU-free): gfx1151 compiles clean (no Cannot-select / LLVM ERROR);
// the one mnemonic that survives the scheduler — s_setprio — is present.
TEST(XpuScheduleAmdDeviceTests, gfx1151IsaCompilesCleanWithSetprio) {
    std::string isa = emitIsaForArch("gfx1151");
    ASSERT_FALSE(isa.empty()) << "ISA emit failed for gfx1151";
    EXPECT_EQ(isa.find("Cannot select"), std::string::npos) << isa;
    EXPECT_EQ(isa.find("LLVM ERROR"), std::string::npos) << isa;
    EXPECT_NE(isa.find("s_setprio"), std::string::npos)
        << "priority(level) must emit s_setprio\n" << isa;
}

// U2.1.c / U2.3 (on-device, skips w/o HIP): a hinted kernel is numerically correct
// — scheduling hints never change results (spec §3.1.3).
TEST(XpuScheduleAmdDeviceTests, hintedKernelRunsCorrectOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kSchedSrc);
    auto method = findMethod(module->getStructures()["test.M"], "sched");
    ASSERT_NE(method, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_sched_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(method, deviceModule), nullptr);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 256;
    std::vector<uint32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = i;

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "sched");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dOut = hip.alloc(std::size_t(n) * sizeof(uint32_t));
    HipDevicePtr dIn = hip.alloc(std::size_t(n) * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(uint32_t)));

    void* params[] = { &dOut, &dIn };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/256, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(n, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut, std::size_t(n) * sizeof(uint32_t)));
    hip.free(dOut);
    hip.free(dIn);

    for (uint32_t i = 0; i < n; ++i)
        EXPECT_EQ(out[i], 255u - i) << "lane " << i << " (hints changed the result?)";
}
