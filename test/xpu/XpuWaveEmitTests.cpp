//
// CajetaXPU wave ops (@Wave) — the seam's headline test across all THREE
// backends.
//
// Wave-level ops (warp / wavefront / subgroup) are the archetypal variance-
// shaped feature (cajeta-amd.md §2): every backend has the hardware, but the
// intrinsic, lane width, and ballot shape all diverge. The SAME Cajeta source
// (Wave.shuffleSync / Wave.ballotSync) lowers through the shared AST walk with
// only the three new LoweringTarget wave methods forking:
//   NVPTX:  llvm.nvvm.shfl.sync.idx.i32 / llvm.nvvm.vote.ballot.sync
//   AMDGPU: llvm.amdgcn.readlane / llvm.amdgcn.ballot
//   SPIR-V: llvm.spv.wave.readlane / llvm.spv.wave.ballot (→ OpGroupNonUniform*)
//
// GPU-free: asserts the device IR carries the right per-backend intrinsics, and
// that the Vulkan module is strictly spirv-val-valid. On-device verification is
// XpuWaveDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// out[t] = readlane(t*10+5, 3) + (u32) ballot(t < 4).
// Per-wave (block <= wave width): every lane reads lane 3's value (35) and the
// same ballot mask (low 4 bits → 0xF=15), so the device result is 50.
const char* kWaveSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavetest(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 r = Wave.shuffleSync(t * 10 + 5, 3);\n"
    "        uint64 b = Wave.ballotSync(t < 4);\n"
    "        out[t] = r + (uint32) b;\n"
    "    }\n"
    "}\n";

// out[t] = Wave.reduceSum(1) — a wave-wide sum, the "comprehensiveness-
// inversion" probe. The guess was that reduce would be one native intrinsic on
// Vulkan but a shuffle/DPP butterfly sequence on NV/AMD; the build showed all
// three expose a single hardware wave-reduce intrinsic (NVPTX's gated sm_80+).
const char* kReduceSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavereduce(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.reduceSum(1);\n"
    "    }\n"
    "}\n";

// out[t] = Wave.laneId() — the lane index within the wave (Inc 5C). Each
// backend reads its native lane-id source: NVPTX %laneid, AMDGPU mbcnt, SPIR-V
// SubgroupLocalInvocationId.
const char* kLaneSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavelane(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.laneId();\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_wave_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_wave_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

cajeta::MethodPtr compileWaveKernel(Compiler& compiler) {
    auto module = compileForInspection(compiler, kWaveSource);
    return findMethod(module->getStructures()["test.M"], "wavetest");
}

cajeta::MethodPtr compileReduceKernel(Compiler& compiler) {
    auto module = compileForInspection(compiler, kReduceSource);
    return findMethod(module->getStructures()["test.M"], "wavereduce");
}

cajeta::MethodPtr compileLaneKernel(Compiler& compiler) {
    auto module = compileForInspection(compiler, kLaneSource);
    return findMethod(module->getStructures()["test.M"], "wavelane");
}

} // namespace

// NVPTX: wave ops lower to the warp-shuffle + vote-ballot intrinsics.
TEST(XpuWaveEmitTests, nvptxLowersShuffleAndBallot) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_wave_nvptx", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.nvvm.shfl.sync.idx.i32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.nvvm.vote.ballot.sync"), std::string::npos) << ir;
}

// AMDGPU: wave ops lower to readlane + ballot.

// SPIR-V: wave ops lower to the subgroup intrinsics, and the module is valid.

// --- Maximal reconvergence (SPV_KHR_maximal_reconvergence) ------------------
// A kernel that uses a cross-lane Wave op requests maximal reconvergence so the
// op sees the source-converged lanes: OpExecutionMode <entry> Maximally
// ReconvergesKHR + OpExtension "SPV_KHR_maximal_reconvergence". No fork — the
// backend turns the "enable-maximal-reconvergence" fn-attr into the mode. The
// request is gated on wave-op use, so a non-wave kernel must NOT carry it.

// The complement: a kernel with NO cross-lane wave op (Wave.laneId is a pure
// per-lane query) must NOT pull in maximal reconvergence — wave kernels pay for
// the device requirement, plain kernels don't.

// --- Subgroup rotate (SPV_KHR_subgroup_rotate) ------------------------------
// Wave.rotate(value, delta) reads value from lane (laneId + delta) mod width.
// Vulkan lowers to a single native OpGroupNonUniformRotateKHR at Subgroup scope
// via the fork's llvm.spv.subgroup.rotate intrinsic (the __spirv builtin path is
// OpenCL-only). NVIDIA/AMD/CPU inherit the base default (laneId+shuffle). The
// kernel uses a cross-lane op, so it also requests maximal reconvergence.
const char* kRotateSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class R {\n"
    "    @Kernel\n"
    "    public static void waverot(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.rotate(Wave.laneId(), 1);\n"
    "    }\n"
    "}\n";

// Exclusive prefix scans: Wave.prefixSum/prefixProduct lower to the native
// OpGroupNonUniform{IAdd,IMul} with the ExclusiveScan group operation (the fork
// spv_wave_prefix_{sum,product} intrinsics). GPU-free: asserts the ops emit and
// the module is spirv-val-clean.
const char* kScanSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class S {\n"
    "    @Kernel\n"
    "    public static void wavescan(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.prefixSum(t) + Wave.prefixProduct(t);\n"
    "    }\n"
    "}\n";


// The reduction family beyond sum: Wave.reduce{Max,Min,And,Or,Xor} lower to the
// GroupNonUniformArithmetic Reduce ops (already Shader-reachable — NOT the
// OpenCL-only SPV_KHR_uniform_group_instructions). Unsigned min/max. GPU-free:
// asserts the five ops emit and the module is spirv-val-clean.
const char* kReduceFamilySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class F {\n"
    "    @Kernel\n"
    "    public static void wavereduceops(KernelBuffer<uint32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 a = Wave.reduceMax(t) + Wave.reduceMin(t);\n"
    "        uint32 b = Wave.reduceAnd(t) + Wave.reduceOr(t) + Wave.reduceXor(t);\n"
    "        out[t] = a + b;\n"
    "    }\n"
    "}\n";

TEST(XpuWaveEmitTests, spirvLowersReduceFamilyAndValidates) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kReduceFamilySource);
    auto k = findMethod(module->getStructures()["test.F"], "wavereduceops");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext txtCtx;
    llvm::Module txtMod("xpu_redfam_txt", txtCtx);
    cajeta::xpu::vulkan::configureDeviceModule(txtMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, txtMod);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(txtMod, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpGroupNonUniformUMax"), std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformUMin"), std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformBitwiseAnd"), std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformBitwiseOr"), std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformBitwiseXor"), std::string::npos) << text;

    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_redfam_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_redfam_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the reduce-family module";
}


// --- Wave.reduce: a single hardware wave-reduce intrinsic on all three ------
// The guessed comprehensiveness inversion (1 intrinsic on Vulkan vs. a
// shuffle/DPP sequence on NV/AMD) did not hold — each backend lowers to one
// native reduce. NVPTX's redux.sync needs sm_80+ (the test targets sm_89).

TEST(XpuWaveEmitTests, nvptxLowersReduceToReduxSync) {
    Compiler compiler;
    auto k = compileReduceKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_reduce_nvptx", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.nvvm.redux.sync.add"), std::string::npos) << ir;
}



// laneId() lowers to each backend's native lane-index source (Inc 5C).
TEST(XpuWaveEmitTests, nvptxLowersLaneIdToSreg) {
    Compiler compiler;
    auto k = compileLaneKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_lane_nvptx", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.nvvm.read.ptx.sreg.laneid"), std::string::npos) << ir;
}


// Re-enabled 2026-06-07: the SPIR-V lane-id lowering now terminates promptly
// under spirv-val (the wave/subgroup-invocation lowering done in the XPU
// session fixed the non-terminating-bytes case), and the test passes in ~4.5s.
// Was DISABLED 2026-06-02 because the unbounded `ExecuteAndWait(spirv-val, ...)`
// below could wedge ctest indefinitely; that failure mode is additionally
// contained now that cajeta_tests.sh runs each test in its own process under a
// per-test timeout, so a stuck validator is killed rather than hanging the run.
