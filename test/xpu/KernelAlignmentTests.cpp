//
// KernelAlignmentTests — the shared alignment proof (kernel-load-alignment
// plan Unit 1, spec 2.2-2.6). One proof answers for NVPTX, AMDGPU and
// Vulkan, so its algebra is pinned here on IR, backend-free: the NVPTX
// cases it moved with (NvptxWordFormTests keeps the PTX side), the merges
// and masks the llm's hand-aligned offsets need, and the 16-byte cap.
// Each "fires" case has a "does not fire" beside it.
//
#include <gtest/gtest.h>

#include "cajeta/xpu/lowering/AlignmentProof.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "../jit/JitTestHelper.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include <memory>
#include <sstream>
#include <string>

namespace {

// The proven alignment of the first load in `@f`, whose address is derived
// from argument 0, with the base aligned to 16 bytes (the launch contract).
uint64_t provenOfFirstLoad(const std::string& body) {
    static llvm::LLVMContext ctx;
    llvm::SMDiagnostic err;
    std::string ir = "target datalayout = \"e-i64:64-v16:16-v32:32-n16:32:64\"\n" + body;
    std::unique_ptr<llvm::Module> m = llvm::parseAssemblyString(ir, err, ctx);
    if (!m) {
        ADD_FAILURE() << "IR did not parse: " << err.getMessage().str();
        return 0;
    }
    llvm::Function* f = m->getFunction("f");
    for (llvm::BasicBlock& bb : *f)
        for (llvm::Instruction& i : bb)
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&i))
                return cajeta::xpu::provenAlignment(ld->getPointerOperand(), f->getArg(0),
                                                    16, m->getDataLayout());
    ADD_FAILURE() << "no load in @f";
    return 0;
}

}  // namespace

// 1.1.1: a product, a sum of aligned terms (the Q4_K row offset's shape).
TEST(KernelAlignment, aProductAndASumOfAlignedTermsAreProven) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = mul i64 %i, 32
  %o2 = add i64 %o, 16
  %p = getelementptr i8, ptr %b, i64 %o2
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.1, does not fire: an index nothing proves stays at one byte (spec 2.2).
TEST(KernelAlignment, aRuntimeOffsetProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %off) {
  %o = add i64 %i, %off
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.1: a loop-carried offset is proven from its start and its step.
TEST(KernelAlignment, aLoopCarriedOffsetIsProvenFromItsStartAndStep) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %n) {
entry:
  %s = mul i64 %i, 64
  br label %loop
loop:
  %o = phi i64 [ %s, %entry ], [ %o2, %loop ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  %o2 = add i64 %o, 32
  %c = icmp ult i64 %o2, %n
  br i1 %c, label %loop, label %done
done:
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.1, does not fire: an aligned step from an unproven start proves nothing.
TEST(KernelAlignment, aLoopCarriedOffsetFromAnUnprovenStartProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %start, i64 %n) {
entry:
  br label %loop
loop:
  %o = phi i64 [ %start, %entry ], [ %o2, %loop ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  %o2 = add i64 %o, 32
  %c = icmp ult i64 %o2, %n
  br i1 %c, label %loop, label %done
done:
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.2: a select of a 16- and a 4-aligned value proves 4 (spec 2.3).
TEST(KernelAlignment, aSelectProvesTheSmallerArm) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
  %a = mul i64 %i, 16
  %w = mul i64 %j, 4
  %o = select i1 %c, i64 %a, i64 %w
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 4u);
}

// 1.1.2: an if-merge (phi) of a 16- and a 4-aligned value proves 4.
TEST(KernelAlignment, anIfMergeProvesTheSmallerArm) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
entry:
  br i1 %c, label %t, label %e
t:
  %a = mul i64 %i, 16
  br label %m
e:
  %w = mul i64 %j, 4
  br label %m
m:
  %o = phi i64 [ %a, %t ], [ %w, %e ]
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 4u);
}

// 1.1.2, does not fire: a merge with an unproven arm proves nothing.
TEST(KernelAlignment, aMergeWithAnUnprovenArmProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i, i64 %j, i1 %c) {
  %a = mul i64 %i, 16
  %o = select i1 %c, i64 %a, i64 %j
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.3: a hand mask (the llm's a16) proves its alignment whatever the index
// (spec 2.4), so the existing masks stay correct.
TEST(KernelAlignment, aMaskOfMinusSixteenProvesSixteen) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = and i64 %i, -16
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}

// 1.1.3, does not fire: a low-bit mask proves nothing.
TEST(KernelAlignment, aMaskOfFifteenProvesNothing) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = and i64 %i, 15
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 1u);
}

// 1.1.4: a product by 64 is credited 16, the widest access (spec 2.6).
TEST(KernelAlignment, aProvenAlignmentAboveSixteenIsSixteen) {
    EXPECT_EQ(provenOfFirstLoad(R"(
define <16 x i8> @f(ptr %b, i64 %i) {
  %o = mul i64 %i, 64
  %p = getelementptr i8, ptr %b, i64 %o
  %v = load <16 x i8>, ptr %p, align 1
  ret <16 x i8> %v
})"), 16u);
}

// ---------------------------------------------------------------------------
// The stated form, `vload<N>(i, align: A)` / `vstore(i, v, align: A)`
// (kernel-load-alignment plan Unit 4, spec 3.1-3.5).
// ---------------------------------------------------------------------------

namespace {

using namespace cajeta::xpu::probe;

// `off` is a parameter, so nothing proves `i * 16 + off` aligned; the
// statement is the only source of the alignment.
std::string statedSource(const std::string& call) {
    return std::string(
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, KernelBuffer<int8> x,\n"
        "            uint32 n, uint32 off) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            int64 o = (int64) (i * 16 + off);\n"
        "            int64 p = (int64) i * 16L;\n"
        "            uint32 m = n;\n"
        "            ") + call + "\n"
        "            Vector<int32,4> zv = heap Vector<int32,4>(0, 0, 0, 0);\n"
        "            Vector<int32,4> t = v.dotAccum(v, zv);\n"
        "            out[(int64) i] = t[0] + t[1] + t[2] + t[3];\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
}

std::string vulkanText(const std::string& src) {
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M", "stated_vk");
    auto k = findMethod(module->getStructures()["test.M"], "k");
    if (!k) return "";
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext ctx;
    llvm::Module m("stated_vk", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(m, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, m);
    return cajeta::xpu::vulkan::emitSpirvText(m, *tm);
}

const char* kStated = "Vector<int8,16> v = x.vload<16>(o, align: 16);";

// Byte loads in the PTX: `ld.*` lines of an 8-bit type. A plain `.b8` count
// also matches a global's own declaration (the fault record is `.global .b8`).
size_t byteLoads(const std::string& ptx) {
    size_t n = 0;
    std::istringstream in(ptx);
    for (std::string line; std::getline(in, line);) {
        const auto t = line.find_first_not_of(" \t");
        if (t == std::string::npos || line.compare(t, 3, "ld.") != 0) continue;
        if (line.find(".u8") != std::string::npos || line.find(".s8") != std::string::npos
            || line.find(".b8") != std::string::npos)
            ++n;
    }
    return n;
}

}  // namespace

// 4.1.1 (spec 3.1), NVPTX: the stated 16 makes the load one 16-byte vector.
TEST(KernelAlignment, aStatedAlignmentWidensTheLoadOnNvptx) {
    const Lowered l = lowerForNvptx(statedSource(kStated), "k", "test.M", "sm_89", "stated");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(byteLoads(l.ptx), 0u) << l.ptx;
    EXPECT_GE(countOf(l.ptx, ".v4.b32") + countOf(l.ptx, ".v4.u32"), 1u) << l.ptx;
}

// 4.1.1 (spec 3.1), NVPTX, does not fire: without the statement the same load
// stays bytes (nothing proves `o`).
TEST(KernelAlignment, withoutAStatementTheUnprovableLoadStaysBytesOnNvptx) {
    const Lowered l = lowerForNvptx(
        statedSource("Vector<int8,16> v = x.vload<16>(o);"), "k", "test.M", "sm_89", "stated0");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(byteLoads(l.ptx), 16u) << l.ptx;
}

// 4.1.1 (spec 3.1), AMDGPU: the load is emitted at the stated alignment.
TEST(KernelAlignment, aStatedAlignmentReachesTheAmdgpuLoad) {
    const Lowered l = lowerForAmdgpu(statedSource(kStated), "k", "test.M", "gfx1151", "stated_amd");
    ASSERT_TRUE(l.ok) << l.why;
    // The pipeline may retype the byte vector (to <4 x i32>); the access is the
    // `vload` value, whatever its type.
    bool aligned = false;
    std::istringstream in(l.ir);
    for (std::string line; std::getline(in, line);)
        if (line.find("%vload") != std::string::npos && line.find(" load ") != std::string::npos
            && line.find("addrspace(1)") != std::string::npos
            && line.find("align 16") != std::string::npos)
            aligned = true;
    EXPECT_TRUE(aligned) << l.ir;
}

// 4.1.1 (spec 3.1, 4.2), Vulkan: the statement reaches Mesa as an index mask,
// the one alignment form it reads (Julian, 2026-10-28: the fork's backend
// writes no SPIR-V `Aligned` operand).
TEST(KernelAlignment, aStatedAlignmentReachesTheVulkanLoadAsAnIndexMask) {
    std::string text = vulkanText(statedSource(kStated));
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpBitwiseAnd"), std::string::npos) << text;
}

// Does not fire: without the statement nothing proves `o`, so no mask.
TEST(KernelAlignment, withoutAStatementTheVulkanIndexIsNotMasked) {
    std::string text = vulkanText(statedSource("Vector<int8,16> v = x.vload<16>(o);"));
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("OpBitwiseAnd"), std::string::npos) << text;
}

// 4.1.1 (spec 3.1), NVPTX: a statement below 16 still lifts the unprovable load
// out of bytes, into words.
TEST(KernelAlignment, aStatedWordAlignmentMakesWordLoadsOnNvptx) {
    const Lowered l = lowerForNvptx(
        statedSource("Vector<int8,16> v = x.vload<16>(o, align: 4);"), "k", "test.M", "sm_89",
        "statedw");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(byteLoads(l.ptx), 0u) << l.ptx;
    EXPECT_EQ(countOf(l.ptx, ".v4.b32") + countOf(l.ptx, ".v4.u32"), 0u) << l.ptx;
}

// 4.1.3 (spec 3.3): the proof has 16 and the statement says 4; 16 is used.
TEST(KernelAlignment, aSmallerStatementDoesNotLowerTheProvenAlignment) {
    const Lowered l = lowerForNvptx(
        statedSource("Vector<int8,16> v = x.vload<16>(p, align: 4);"), "k", "test.M", "sm_89",
        "stated4");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_GE(countOf(l.ptx, ".v4.b32") + countOf(l.ptx, ".v4.u32"), 1u) << l.ptx;
}

// 4.1.2 (spec 3.2, 3.4): each bad statement fails the kernel, naming the call.
TEST(KernelAlignment, aBadStatementFailsNamingTheCall) {
    struct Case { const char* call; const char* why; };
    const Case cases[] = {
        {"Vector<int8,16> v = x.vload<16>(o, align: 3);", "power of two"},
        {"Vector<int8,16> v = x.vload<16>(o, align: 32);", "power of two"},
        {"Vector<int8,16> v = x.vload<16>(o, align: m);", "constant"},
        {"Vector<int8,4> w = x.vload<4>(o, align: 16); Vector<int8,16> v = x.vload<16>(p);",
         "wider than"},
        {"Vector<int8,16> v = x.vload<16>(o, aligned: 16);", "align:"},
    };
    for (const Case& c : cases) {
        const Lowered l = lowerForNvptx(statedSource(c.call), "k", "test.M", "sm_89", "statedbad");
        EXPECT_FALSE(l.ok) << c.call;
        EXPECT_NE(l.why.find("vload"), std::string::npos) << c.call << ": " << l.why;
        EXPECT_NE(l.why.find(c.why), std::string::npos) << c.call << ": " << l.why;
        EXPECT_NE(l.why.find("line"), std::string::npos) << c.call << ": " << l.why;
    }
}

// 4.1.2 (spec 3.5): outside a kernel the existing kernel-only error applies.
TEST(KernelAlignment, aStatementOutsideAKernelIsTheKernelOnlyError) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public class H {\n"
        "    public static int32 run() {\n"
        "        KernelBuffer<int8> x = heap KernelBuffer<int8>(0, 64);\n"
        "        Vector<int8,16> v = x.vload<16>(0L, align: 16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    std::string err;
    try {
        auto jit = cajeta_test::CajetaJit::compile(src, "test.H");
        EXPECT_EQ(jit, nullptr) << "a host vload compiled";
    } catch (cajeta::Exception& e) {
        err = e.getMessage();
    } catch (const std::exception& e) {
        err = e.what();
    }
    if (!err.empty()) EXPECT_NE(err.find("kernel-only"), std::string::npos) << err;
}

// ---------------------------------------------------------------------------
// The check (kernel-load-alignment plan Unit 5, spec 4.1-4.3). A build with
// ubTraps on (debug, or release with --ub-traps) tests every statement the
// proof cannot confirm; a violation is recorded in the kernel's fault record
// and the access proceeds at the index rounded down (Julian, 2026-10-28: a
// fault record read at sync, never a device trap).
// ---------------------------------------------------------------------------

namespace {

cajeta::CompilerFlags flagsFor(cajeta::CompilerMode mode, bool ubTraps) {
    cajeta::CompilerFlags f = cajeta::CompilerFlags::defaultsForMode(mode);
    f.ubTraps = ubTraps;
    return f;
}

bool carriesTheCheck(const Lowered& l) {
    return l.ptx.find("__cajeta_xpu_align_fault") != std::string::npos;
}

}  // namespace

// 5.1.1 (spec 4.1): a debug build records a statement nothing proves.
TEST(KernelAlignment, aDebugBuildChecksAStatementNothingProves) {
    const Lowered l = lowerForNvptx(statedSource(kStated), "k", "test.M", "sm_89", "chk_dbg",
                                    flagsFor(cajeta::CompilerMode::Debug, true));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_TRUE(carriesTheCheck(l)) << l.ptx;
    EXPECT_GE(countOf(l.ptx, ".cas.b64"), 1u) << l.ptx;
}

// 5.1.2 (spec 4.1): a release build with ubTraps on checks it too.
TEST(KernelAlignment, aReleaseBuildWithUbTrapsChecksAStatement) {
    const Lowered l = lowerForNvptx(statedSource(kStated), "k", "test.M", "sm_89", "chk_rel1",
                                    flagsFor(cajeta::CompilerMode::Release, true));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_TRUE(carriesTheCheck(l)) << l.ptx;
}

// 5.1.3 (spec 4.2): a release build without ubTraps carries no check.
TEST(KernelAlignment, aReleaseBuildWithoutUbTrapsCarriesNoCheck) {
    const Lowered l = lowerForNvptx(statedSource(kStated), "k", "test.M", "sm_89", "chk_rel0",
                                    flagsFor(cajeta::CompilerMode::Release, false));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_FALSE(carriesTheCheck(l)) << l.ptx;
}

// 5.1.4 (spec 4.3): a statement the proof confirms adds no check, even in debug.
TEST(KernelAlignment, aStatementTheProofConfirmsCarriesNoCheck) {
    const Lowered l = lowerForNvptx(
        statedSource("Vector<int8,16> v = x.vload<16>(p, align: 16);"), "k", "test.M", "sm_89",
        "chk_proven", flagsFor(cajeta::CompilerMode::Debug, true));
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_FALSE(carriesTheCheck(l)) << l.ptx;
}

namespace {

// `off` moves every work-item's index off the statement's 16-byte grid.
const char* kCpuViolation =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void stated(KernelBuffer<int32> out, KernelBuffer<int8> x, uint32 off) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        int64 o = (int64) (i * 16 + off);\n"
    "        Vector<int8,16> v = x.vload<16>(o, align: 16);\n"
    "        out[(int64) i] = (int32) v[0] + (int32) v[15];\n"
    "    }\n"
    "    /** 1 when the launch threw, plus 2/4/8/16 for each part of the message\n"
    "     *  that names the call: the statement, its line, the index, the kernel;\n"
    "     *  100 + out[1] when it did not throw. */\n"
    "    public static int32 run(uint32 off) {\n"
    "        int8[] h = heap int8[256];\n"
    "        for (int32 j = 0; j < 256; ++j) { h[j] = (int8) (j & 63); }\n"
    "        KernelBuffer<int8> x = heap KernelBuffer<int8>(256);\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);\n"
    "        x.upload(h);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 r = 0;\n"
    "        try {\n"
    "            stated.launch(s, grid: [1], block: [4])(out, x, off);\n"
    "            s.sync();\n"
    "            int32[] o = heap int32[4];\n"
    "            out.download(o);\n"
    "            r = 100 + o[1];\n"
    "        } catch (XpuLaunchException e) {\n"
    "            String m = e.getMessage();\n"
    "            System.stderr.println(m);\n"
    "            r = 1;\n"
    "            if (m.contains(\"align: 16\")) { r = r + 2; }\n"
    "            if (m.contains(\"line 11\")) { r = r + 4; }\n"
    "            if (m.contains(\"index 4\")) { r = r + 8; }\n"
    "            if (m.contains(\"stated\")) { r = r + 16; }\n"
    "        }\n"
    "        return r;\n"
    "    }\n"
    "}\n";

}  // namespace

// 5.1.1 (spec 4.1), cpu: a violated statement raises at the launch, naming the
// kernel, the call's line and the index; the next conforming launch runs.
TEST(KernelAlignment, aViolatedStatementRaisesNamingTheCallOnCpu) {
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = cajeta_test::CajetaJit::compile(kCpuViolation, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)(uint32_t)>("run");
    ASSERT_NE(run, nullptr);
    // Work-item 0 reads at element 4: off the 16-byte grid.
    EXPECT_EQ(run(4), 1 + 2 + 4 + 8 + 16);
    // Conforming: work-item 1 reads bytes 16..31, so v[0] + v[15] = 16 + 31.
    EXPECT_EQ(run(0), 100 + 16 + 31);
}

namespace {

void raisesOnDevice(cajeta::xpu::Backend be) {
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = cajeta_test::CajetaJit::compile(kCpuViolation, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)(uint32_t)>("run");
    ASSERT_NE(run, nullptr);
    // A device runs the four work-items at once, so any one may be the first
    // to break the statement: index 4, 20, 36 or 52. The message names one.
    const int32_t r = run(4);
    EXPECT_TRUE(r == 1 + 2 + 4 + 8 + 16 || r == 1 + 2 + 4 + 16) << r;
    // The record is read after the launch, never a trap: the context is
    // intact and the next conforming launch runs.
    EXPECT_EQ(run(0), 100 + 16 + 31);
}

}  // namespace

// 5.1.1 (spec 4.1), CUDA: the fault record is read back after the launch.
TEST(KernelAlignment, aViolatedStatementRaisesNamingTheCallOnCuda) {
    CAJETA_SKIP_IF_NO_CUDA();
    raisesOnDevice(cajeta::xpu::Backend::Nvptx);
}

// 5.2.2 (spec 4.1), Vulkan: the record is a storage binding after the
// kernel's own, read back once the launch's batch is flushed.
TEST(KernelAlignment, aViolatedStatementRaisesNamingTheCallOnVulkan) {
    CAJETA_SKIP_IF_NO_VULKAN();
    raisesOnDevice(cajeta::xpu::Backend::Spirv);
}

// 5.1.1 (spec 4.1), HIP: the same record, read with the hip calls.
TEST(KernelAlignment, aViolatedStatementRaisesNamingTheCallOnHip) {
    CAJETA_SKIP_IF_NO_HIP();
    raisesOnDevice(cajeta::xpu::Backend::Amdgpu);
}

// ---------------------------------------------------------------------------
// Views (kernel-load-alignment plan Unit 6, spec 5.1, 5.2). Vulkan binds a view
// as its storage buffer's descriptor offset, which the device allows only at
// multiples of minStorageBufferOffsetAlignment (16 on llvmpipe); a view
// elsewhere is refused by name rather than bound as invalid usage.
// ---------------------------------------------------------------------------

namespace {

const char* kViewSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void copy(KernelBuffer<int32> out, KernelBuffer<int8> x) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        out[(int64) i] = (int32) x[(int64) i];\n"
    "    }\n"
    "    /** 1 + 2/4/8 for the message naming the argument, the offset and the\n"
    "     *  limit when the launch over a view at `off` threw; 100 + out[3]\n"
    "     *  when it ran. */\n"
    "    public static int32 run(int64 off) {\n"
    "        int8[] h = heap int8[256];\n"
    "        for (int32 j = 0; j < 256; ++j) { h[j] = (int8) (j & 63); }\n"
    "        KernelBuffer<int8> x = heap KernelBuffer<int8>(256);\n"
    "        x.upload(h);\n"
    "        KernelBuffer<int8> v #= x.slice((uint64) off, (uint64) 64);\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 r = 0;\n"
    "        try {\n"
    "            copy.launch(s, grid: [1], block: [4])(out, v);\n"
    "            s.sync();\n"
    "            int32[] o = heap int32[4];\n"
    "            out.download(o);\n"
    "            r = 100 + o[3];\n"
    "        } catch (XpuLaunchException e) {\n"
    "            String m = e.getMessage();\n"
    "            System.stderr.println(m);\n"
    "            r = 1;\n"
    "            if (m.contains(\"argument 1\")) { r = r + 2; }\n"
    "            if (m.contains(\"offset 1,\")) { r = r + 4; }\n"
    "            if (m.contains(\"only at a multiple of \")) { r = r + 8; }\n"
    "        }\n"
    "        return r;\n"
    "    }\n"
    "}\n";

}  // namespace

// 6.1.1 (spec 5.1, 5.2), Vulkan: a view at an offset the device cannot bind is
// refused naming the argument, the offset and the limit; one at an allowed
// offset runs.
TEST(KernelAlignment, aVulkanViewAtAnUnbindableOffsetIsRefusedByName) {
    CAJETA_SKIP_IF_NO_VULKAN();
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = cajeta_test::CajetaJit::compile(kViewSource, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)(int64_t)>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(16), 100 + (19 & 63));   // allowed: element 3 of the view is byte 19
    // Byte 1 is unbindable on every device: the limit is a power of two of at
    // least 4 in practice (RADV 4, llvmpipe and NVIDIA 16). Offset 4 was legal on
    // RADV, so this test failed on proton (Proton, 2026-10-29).
    EXPECT_EQ(run(1), 1 + 2 + 4 + 8);
    EXPECT_EQ(run(32), 100 + (35 & 63));   // and the next allowed one still runs
}
