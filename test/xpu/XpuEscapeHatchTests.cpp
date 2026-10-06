// The escape hatch: a library reaches the hardware before the compiler has a
// building block for it (xpu-kernel-independence Unit 11, spec §5, §7.5).
//
// A @Device helper names its backend arms in an annotation. @TargetIntrinsic
// names an LLVM intrinsic per backend, called with the helper's own
// signature, which the compiler checks against the intrinsic's. @TargetAsm
// names an inline-assembly template and its constraints per backend, checked
// at the boundary: one constraint per operand, each admitting the operand's
// type. The helper's body, when it has one, is the portable arm, and is what
// the reference interpreter runs, so the corpus checks an intrinsic arm
// against it (§5.2). A backend with neither an arm nor a portable arm refuses
// the kernel by name.
#include "gtest/gtest.h"

#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "XpuRefusalProbe.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/reference/Conformance.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

// The portable arm of a byte permute: result byte i is byte (nibble i of
// `sel`, 0..7) of the pair {lo, hi}, what PTX's prmt.b32 and AMDGCN's
// v_perm_b32 compute for selectors without the sign-replicate bit.
const char* kPermuteBody = R"CJ(
        uint32 r = 0;
        for (uint32 i = 0; i < 4; i = i + 1) {
            uint32 n = (sel >> (4 * i)) & 7;
            uint32 b = n < 4 ? (lo >> (8 * n)) & 255 : (hi >> (8 * (n - 4))) & 255;
            r = r | (b << (8 * i));
        }
        return r;
)CJ";

std::string program() {
    return std::string(R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.XpuLaunchException;
public class M {
    @Device
    @TargetIntrinsic(nvptx = "llvm.nvvm.prmt")
    public static uint32 permute(uint32 lo, uint32 hi, uint32 sel) {)CJ") + kPermuteBody + R"CJ(    }
    @Device
    @TargetAsm(nvptx = "prmt.b32 $0, $1, $2, $3;", nvptxConstraints = "=r,r,r,r",
               amdgpu = "v_perm_b32 $0, $2, $1, $3", amdgpuConstraints = "=v,v,v,v")
    public static uint32 permuteAsm(uint32 lo, uint32 hi, uint32 sel) {)CJ" + kPermuteBody + R"CJ(    }
    @Kernel
    public static void viaIntrinsic(KernelBuffer<uint32> y, KernelBuffer<uint32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = M.permute(x[i], x[i] ^ 0x5A5A5A5A, (i * 0x1111) & 0x7777); }
    }
    @Kernel
    public static void viaAsm(KernelBuffer<uint32> y, KernelBuffer<uint32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = M.permuteAsm(x[i], x[i] ^ 0x5A5A5A5A, (i * 0x1111) & 0x7777); }
    }
    public static uint32 run(uint32 which, uint32 at) {
        uint32[] h = heap uint32[256];
        for (uint32 i = 0; i < 256; i = i + 1) { h[i] = i * 0x01030507 + 0x11223344; }
        KernelBuffer<uint32> x = heap KernelBuffer<uint32>(256);
        KernelBuffer<uint32> y = heap KernelBuffer<uint32>(256);
        x.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        if (which == 0) { viaIntrinsic.launch(s, grid: [8], block: [32])(y, x, 256); }
        else { viaAsm.launch(s, grid: [8], block: [32])(y, x, 256); }
        s.sync();
        y.download(h);
        return h[at];
    }
    public static int32 runAll() {
        uint32[] h = heap uint32[256];
        for (uint32 i = 0; i < 256; i = i + 1) { h[i] = i * 0x01030507 + 0x11223344; }
        KernelBuffer<uint32> x = heap KernelBuffer<uint32>(256);
        KernelBuffer<uint32> y = heap KernelBuffer<uint32>(256);
        x.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        viaIntrinsic.launch(s, grid: [8], block: [32])(y, x, 256);
        s.sync();
        viaAsm.launch(s, grid: [8], block: [32])(y, x, 256);
        s.sync();
        return 1;
    }
}
)CJ";
}

// A helper with an nvptx arm and no body: there is nothing to run elsewhere.
std::string armOnlyProgram() {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.XpuLaunchException;
public class M {
    @TargetIntrinsic(nvptx = "llvm.nvvm.prmt")
    public static uint32 prmtOnly(uint32 lo, uint32 hi, uint32 sel);
    @Kernel
    public static void onlyArm(KernelBuffer<uint32> y, KernelBuffer<uint32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = M.prmtOnly(x[i], x[i] ^ 0x5A5A5A5A, (i * 0x1111) & 0x7777); }
    }
    public static int32 probe() {
        uint32[] h = heap uint32[256];
        KernelBuffer<uint32> x = heap KernelBuffer<uint32>(256);
        KernelBuffer<uint32> y = heap KernelBuffer<uint32>(256);
        x.upload(h);
        y.upload(h);
        KernelStream s #= KernelStream.current();
        try {
            onlyArm.launch(s, grid: [8], block: [32])(y, x, 256);
            s.sync();
        } catch (XpuLaunchException e) { }
        return 1;
    }
}
)CJ";
}

// A kernel calling one helper, spelled by `decl` (annotation + signature +
// body or `;`), with `call` its call on (a, b, c) stored into y[i].
std::string oneHelper(const std::string& decl, const std::string& call) {
    return "package test;\nimport cajeta.xpu.KernelBuffer;\nimport cajeta.xpu.KernelThread;\n"
           "public class M {\n" + decl + "\n"
           "    @Kernel\n"
           "    public static void k(KernelBuffer<uint32> y, KernelBuffer<uint32> x, uint32 n) {\n"
           "        uint32 i = KernelThread.globalIdX();\n"
           "        if (i < n) { y[i] = " + call + "; }\n"
           "    }\n"
           "}\n";
}

uint32_t permute(uint32_t lo, uint32_t hi, uint32_t sel) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t n = (sel >> (4 * i)) & 7;
        uint32_t b = n < 4 ? (lo >> (8 * n)) & 255 : (hi >> (8 * (n - 4))) & 255;
        r |= b << (8 * i);
    }
    return r;
}

uint32_t expected(uint32_t i) {
    uint32_t x = i * 0x01030507u + 0x11223344u;
    return permute(x, x ^ 0x5A5A5A5Au, (i * 0x1111u) & 0x7777u);
}

// Every element of both kernels on `be`.
void checkEveryElement(cajeta::xpu::Backend be, const char* name) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(program(), "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<uint32_t (*)(uint32_t, uint32_t)>("run");
    ASSERT_NE(run, nullptr);
    for (uint32_t i = 0; i < 256; ++i) {
        EXPECT_EQ(run(0, i), expected(i)) << name << " intrinsic arm, element " << i;
        EXPECT_EQ(run(1, i), expected(i)) << name << " asm arm, element " << i;
    }
}

// Compile `src` for `be` and run its `probe`, returning what the compile and
// the launch printed.
std::string probeOn(const std::string& src, cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.M", o);
    if (jit)
        if (auto fn = jit->lookup<int (*)()>("probe")) fn();
    return testing::internal::GetCapturedStderr();
}

// gfx1151 ISA for kernel `name` of `src`, empty when it does not lower.
std::string amdgpuIsa(const std::string& src, const char* name) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, src);
    auto k = findKernel(module, "test.M", name);
    if (!k) return {};
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_hatch_isa", ctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    try {
        if (!cajeta::xpu::amd::lowerKernel(k, dev)) return {};
    } catch (cajeta::Exception& e) {
        return {};
    }
    return cajeta::xpu::amd::emitIsa(dev, *tm);
}

// Record every launch of `runAll` on `be` into `dir`.
void recordOn(cajeta::xpu::Backend be, const fs::path& dir) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(program(), "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("runAll");
    ASSERT_NE(fn, nullptr);
    setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    int32_t r = fn();
    unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1);
}

// The recordings replay through the reference, which runs the portable arm.
void corpusPassesOn(cajeta::xpu::Backend be, const char* backend) {
    fs::path dir = fs::temp_directory_path() / ("cajeta-hatch-corpus-" + std::string(backend));
    fs::remove_all(dir);
    fs::create_directories(dir);
    recordOn(be, dir);
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, program());
    std::vector<cajeta::MethodPtr> kernels;
    for (const char* n : {"viaIntrinsic", "viaAsm"})
        if (auto k = findKernel(module, "test.M", n)) kernels.push_back(k);
    ASSERT_EQ(kernels.size(), 2u);
    ref::CorpusRun run = ref::runCorpus(kernels, dir.string(), "");
    EXPECT_EQ(run.results.size(), 2u) << "both kernels record on " << backend;
    for (auto& r : run.results)
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    fs::remove_all(dir);
}

} // namespace

// 4.11.1.1: the intrinsic arm on nvptx, the portable arm on cpu, one answer.
TEST(XpuEscapeHatch, theIntrinsicArmRunsOnNvptxThroughTheIntrinsic) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(program(), "viaIntrinsic");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ir.find("llvm.nvvm.prmt"), std::string::npos) << "the arm is the intrinsic call";
    EXPECT_NE(l.ptx.find("prmt.b32"), std::string::npos) << l.ptx;
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    checkEveryElement(cajeta::xpu::Backend::Nvptx, "nvptx");
}

TEST(XpuEscapeHatch, thePortableArmRunsOnCpu) {
    checkEveryElement(cajeta::xpu::Backend::Cpu, "cpu");
}

// 4.11.1.2: no arm and no portable arm refuses the kernel, naming the helper
// and the backend; the arm it does name still lowers.
TEST(XpuEscapeHatch, aBackendWithNoArmAndNoPortableArmRefusesTheKernelByName) {
    std::string err = probeOn(armOnlyProgram(), cajeta::xpu::Backend::Cpu);
    EXPECT_TRUE(cajeta_test::loweringRefused(err)) << err;
    EXPECT_NE(err.find("prmtOnly"), std::string::npos) << err;
    EXPECT_NE(err.find("cpu"), std::string::npos) << err;
    EXPECT_NE(err.find("portable arm"), std::string::npos) << err;
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(armOnlyProgram(), "onlyArm");
    EXPECT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ptx.find("prmt.b32"), std::string::npos);
}

// An intrinsic call is typed: an unknown name and a signature the intrinsic
// does not have are each refused naming the helper and the intrinsic.
TEST(XpuEscapeHatch, anUnknownIntrinsicIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(oneHelper(
        "    @TargetIntrinsic(nvptx = \"llvm.nvvm.no.such.thing\")\n"
        "    public static uint32 nothing(uint32 a);",
        "M.nothing(x[i])"), "k");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("nothing"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("llvm.nvvm.no.such.thing"), std::string::npos) << l.why;
}

TEST(XpuEscapeHatch, anIntrinsicWhoseSignatureDoesNotMatchIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(oneHelper(
        "    @TargetIntrinsic(nvptx = \"llvm.nvvm.prmt\")\n"
        "    public static uint64 wide(uint32 a);",
        "(uint32) M.wide(x[i])"), "k");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("wide"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("llvm.nvvm.prmt"), std::string::npos) << l.why;
}

// 4.11.1.3: an inline PTX arm runs on nvptx, an inline AMDGCN arm lowers on
// amdgpu, and the portable arm serves cpu (thePortableArmRunsOnCpu).
TEST(XpuEscapeHatch, theAsmArmsRunOnNvptxAndLowerOnAmdgpu) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(program(), "viaAsm");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ptx.find("prmt.b32"), std::string::npos) << l.ptx;
    std::string isa = amdgpuIsa(program(), "viaAsm");
    EXPECT_NE(isa.find("v_perm_b32"), std::string::npos) << "amdgpu ISA:\n" << isa;
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    checkEveryElement(cajeta::xpu::Backend::Nvptx, "nvptx");
}

// The boundary check: a constraint that does not admit its operand's type,
// and a constraint list of the wrong length, are each refused by name.
TEST(XpuEscapeHatch, anAsmOperandThatDoesNotMatchItsConstraintIsRefusedByName) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(oneHelper(
        "    @TargetAsm(nvptx = \"cvt.rn.f32.s32 $0, $1;\", nvptxConstraints = \"=r,r\")\n"
        "    public static float32 badCvt(int32 a);",
        "(uint32) M.badCvt((int32) x[i])"), "k");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("badCvt"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("=r"), std::string::npos) << l.why;
    EXPECT_NE(l.why.find("float32"), std::string::npos) << l.why;

    Lowered m = lowerForNvptx(oneHelper(
        "    @TargetAsm(nvptx = \"prmt.b32 $0, $1, $2, $3;\", nvptxConstraints = \"=r,r\")\n"
        "    public static uint32 short3(uint32 lo, uint32 hi, uint32 sel);",
        "M.short3(x[i], x[i], 0x3210)"), "k");
    EXPECT_FALSE(m.ok);
    EXPECT_NE(m.why.find("short3"), std::string::npos) << m.why;
    EXPECT_NE(m.why.find("constraint"), std::string::npos) << m.why;
}

// A helper with arms and no body has nothing to run on the host.
TEST(XpuEscapeHatch, anArmOnlyHelperCalledFromHostCodeIsRefused) {
    std::string src = armOnlyProgram();
    src.replace(src.find("    public static int32 probe()"), 0,
        "    public static uint32 host() { return M.prmtOnly(1, 2, 3); }\n");
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    try {
        CajetaJit::compile(src, "test.M", o);
        ADD_FAILURE() << "a host call of an arm-only helper compiled";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_INTRINSIC_CALLED_ON_HOST") << e.getMessage();
        EXPECT_NE(e.getMessage().find("prmtOnly"), std::string::npos) << e.getMessage();
    }
}

// §5.2: an intrinsic arm is checked against the portable arm through the
// corpus, on every backend that records.
TEST(XpuEscapeHatch, theEscapeHatchKernelsPassTheCorpusOnCpu) {
    corpusPassesOn(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuEscapeHatch, theEscapeHatchKernelsPassTheCorpusOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    corpusPassesOn(cajeta::xpu::Backend::Nvptx, "nvptx");
}
