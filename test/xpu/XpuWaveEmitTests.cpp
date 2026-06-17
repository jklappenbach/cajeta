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
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavetest(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
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
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavereduce(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        out[t] = Wave.reduceSum(1);\n"
    "    }\n"
    "}\n";

// out[t] = Wave.laneId() — the lane index within the wave (Inc 5C). Each
// backend reads its native lane-id source: NVPTX %laneid, AMDGPU mbcnt, SPIR-V
// SubgroupLocalInvocationId.
const char* kLaneSource =
    "package test;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavelane(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
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
TEST(XpuWaveEmitTests, amdgpuLowersReadlaneAndBallot) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_wave_amdgpu", ctx);
    cajeta::xpu::amd::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.amdgcn.readlane"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.ballot"), std::string::npos) << ir;
}

// SPIR-V: wave ops lower to the subgroup intrinsics, and the module is valid.
TEST(XpuWaveEmitTests, spirvLowersSubgroupOpsAndValidates) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_wave_spirv_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.wave.readlane"), std::string::npos) << ir;
    // LLVM 23 renamed the ballot intrinsic spv.wave.ballot → spv.subgroup.ballot.
    EXPECT_NE(ir.find("llvm.spv.subgroup.ballot"), std::string::npos) << ir;

    // Fresh module for emission (it mutates), then spirv-val.
    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_wave_spirv_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());

    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_wave_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    // path::c_str() is const wchar_t* on Windows; go through string() so
    // the StringRef has a char buffer to bind to (held until after the wait).
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the wave-ops module";
}

// --- Maximal reconvergence (SPV_KHR_maximal_reconvergence) ------------------
// A kernel that uses a cross-lane Wave op requests maximal reconvergence so the
// op sees the source-converged lanes: OpExecutionMode <entry> Maximally
// ReconvergesKHR + OpExtension "SPV_KHR_maximal_reconvergence". No fork — the
// backend turns the "enable-maximal-reconvergence" fn-attr into the mode. The
// request is gated on wave-op use, so a non-wave kernel must NOT carry it.
TEST(XpuWaveEmitTests, spirvWaveKernelRequestsMaximalReconvergence) {
    Compiler compiler;
    auto k = compileReduceKernel(compiler);   // uses Wave.reduceSum
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_reconv_text", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(m, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, m);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_maximal_reconvergence\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("MaximallyReconvergesKHR"), std::string::npos) << text;
}

// The complement: a kernel with NO cross-lane wave op (Wave.laneId is a pure
// per-lane query) must NOT pull in maximal reconvergence — wave kernels pay for
// the device requirement, plain kernels don't.
TEST(XpuWaveEmitTests, spirvNonWaveKernelOmitsMaximalReconvergence) {
    Compiler compiler;
    auto k = compileLaneKernel(compiler);     // Wave.laneId only — no cross-lane op
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_reconv_neg_text", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(m, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, m);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("MaximallyReconvergesKHR"), std::string::npos) << text;
}

// --- Subgroup rotate (SPV_KHR_subgroup_rotate) ------------------------------
// Wave.rotate(value, delta) reads value from lane (laneId + delta) mod width.
// Vulkan lowers to a single native OpGroupNonUniformRotateKHR at Subgroup scope
// via the fork's llvm.spv.subgroup.rotate intrinsic (the __spirv builtin path is
// OpenCL-only). NVIDIA/AMD/CPU inherit the base default (laneId+shuffle). The
// kernel uses a cross-lane op, so it also requests maximal reconvergence.
const char* kRotateSource =
    "package test;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class R {\n"
    "    @Kernel\n"
    "    public static void waverot(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        out[t] = Wave.rotate(Wave.laneId(), 1);\n"
    "    }\n"
    "}\n";

// Exclusive prefix scans: Wave.prefixSum/prefixProduct lower to the native
// OpGroupNonUniform{IAdd,IMul} with the ExclusiveScan group operation (the fork
// spv_wave_prefix_{sum,product} intrinsics). GPU-free: asserts the ops emit and
// the module is spirv-val-clean.
const char* kScanSource =
    "package test;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class S {\n"
    "    @Kernel\n"
    "    public static void wavescan(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        out[t] = Wave.prefixSum(t) + Wave.prefixProduct(t);\n"
    "    }\n"
    "}\n";

TEST(XpuWaveEmitTests, spirvLowersPrefixScanAndValidates) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kScanSource);
    auto k = findMethod(module->getStructures()["test.S"], "wavescan");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_scan_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.wave.prefix.sum"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.wave.prefix.product"), std::string::npos) << ir;

    llvm::LLVMContext txtCtx;
    llvm::Module txtMod("xpu_scan_txt", txtCtx);
    cajeta::xpu::vulkan::configureDeviceModule(txtMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, txtMod);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(txtMod, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpGroupNonUniformIAdd"), std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformIMul"), std::string::npos) << text;
    EXPECT_NE(text.find("ExclusiveScan"), std::string::npos) << text;

    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_scan_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_scan_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the prefix-scan module";
}

// The reduction family beyond sum: Wave.reduce{Max,Min,And,Or,Xor} lower to the
// GroupNonUniformArithmetic Reduce ops (already Shader-reachable — NOT the
// OpenCL-only SPV_KHR_uniform_group_instructions). Unsigned min/max. GPU-free:
// asserts the five ops emit and the module is spirv-val-clean.
const char* kReduceFamilySource =
    "package test;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "import cajeta.gpu.Wave;\n"
    "public class F {\n"
    "    @Kernel\n"
    "    public static void wavereduceops(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
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

TEST(XpuWaveEmitTests, spirvLowersRotateToNativeOpAndValidates) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kRotateSource);
    auto k = findMethod(module->getStructures()["test.R"], "waverot");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_rot_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.subgroup.rotate"), std::string::npos) << ir;

    llvm::LLVMContext txtCtx;
    llvm::Module txtMod("xpu_rot_txt", txtCtx);
    cajeta::xpu::vulkan::configureDeviceModule(txtMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, txtMod);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(txtMod, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability GroupNonUniformRotateKHR"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_subgroup_rotate\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformRotateKHR"), std::string::npos) << text;

    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_rot_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_rot_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the subgroup-rotate module";
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

TEST(XpuWaveEmitTests, amdgpuLowersReduceToWaveReduce) {
    Compiler compiler;
    auto k = compileReduceKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_reduce_amdgpu", ctx);
    cajeta::xpu::amd::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.amdgcn.wave.reduce.add"), std::string::npos) << ir;
}

TEST(XpuWaveEmitTests, spirvLowersReduceToSubgroupSumAndValidates) {
    Compiler compiler;
    auto k = compileReduceKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_reduce_spirv_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.wave.reduce.sum"), std::string::npos) << ir;

    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_reduce_spirv_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());

    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_reduce_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    // path::c_str() is const wchar_t* on Windows; hold a std::string so the
    // StringRef binds to a char buffer that outlives the wait.
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the wave-reduce module";
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

TEST(XpuWaveEmitTests, amdgpuLowersLaneIdToMbcnt) {
    Compiler compiler;
    auto k = compileLaneKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_lane_amdgpu", ctx);
    cajeta::xpu::amd::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.amdgcn.mbcnt.lo"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.mbcnt.hi"), std::string::npos) << ir;
}

// Re-enabled 2026-06-07: the SPIR-V lane-id lowering now terminates promptly
// under spirv-val (the wave/subgroup-invocation lowering done in the XPU
// session fixed the non-terminating-bytes case), and the test passes in ~4.5s.
// Was DISABLED 2026-06-02 because the unbounded `ExecuteAndWait(spirv-val, ...)`
// below could wedge ctest indefinitely; that failure mode is additionally
// contained now that cajeta_tests.sh runs each test in its own process under a
// per-test timeout, so a stuck validator is killed rather than hanging the run.
TEST(XpuWaveEmitTests, spirvLowersLaneIdToSubgroupInvocationAndValidates) {
    Compiler compiler;
    auto k = compileLaneKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_lane_spirv_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.subgroup.local.invocation.id"),
              std::string::npos) << ir;

    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_lane_spirv_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());

    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_lane_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    // path::c_str() is const wchar_t* on Windows; hold a std::string so the
    // StringRef binds to a char buffer that outlives the wait.
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the lane-id module";
}
