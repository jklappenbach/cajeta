//
// XpuFuseByDefaultTests — on a GPU backend a kernel's `a * b + c` may fuse
// into one multiply-add, as nvcc and hipcc do by default; @Unfused keeps
// every multiply and add rounding on its own, and the cpu backend never fuses.
//
// Why this exists. Kernel lowering stamped no fast-math flags on a plain
// kernel, and LLVM's backends fuse an fmul into an fadd only when the IR
// allows it, so every float32 kernel written as `c = c + a * b` issued two
// instructions per multiply-add on cuda. Measured 2026-11-09 on the RTX 4090:
// the kernel package's 8x8 register-tile GEMM ran 0.55 ms at 8192x512x2048
// written that way and 0.416 ms with explicit Math.fma, against torch's
// 0.40 ms. The lowering now marks a GPU kernel's float arithmetic `contract`
// (fusion allowed, nothing else relaxed). A fused step rounds once where the
// unfused pair rounds twice, so the last bit may differ from the cpu.
//
// On vulkan the driver compiles the SPIR-V and fuses on its own unless an
// instruction carries NoContraction, so leaving `contract` off is not enough
// there. An @Unfused kernel's float multiplies, adds and subtracts are
// decorated NoContraction. Measured 2026-11-11 on proton: Mesa 26.0.8 to
// 26.2.6 moved three of cajeta-llm's bit-identical wave-32 rows by a few ulps
// with byte-identical SPIR-V.
//
#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

std::string source(bool unfused) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n";
    if (unfused) s += "    @Unfused\n";
    s += "    public static void mad(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
         "                           KernelBuffer<float32> b, KernelBuffer<float32> c, uint32 n) {\n"
         "        uint32 i = KernelThread.globalIdX();\n"
         "        if (i < n) { out[i] = a[i] * b[i] + c[i]; }\n"
         "    }\n"
         "}\n";
    return s;
}

std::string print(llvm::Module& m) {
    std::string ir;
    llvm::raw_string_ostream os(ir);
    m.print(os, nullptr);
    os.flush();
    return ir;
}

std::string cpuIr(bool unfused) {
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    if (!tm) return {};
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source(unfused), "test.M", "fuse");
    auto k = findMethod(module->getStructures()["test.M"], "mad");
    if (!k) return {};
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_fuse_cpu", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    if (!cajeta::xpu::cpu::lowerKernel(k, host)) return {};
    return print(host);
}

std::string spirvIr(bool unfused) {
    using namespace cajeta::xpu::vulkan;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) return {};
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source(unfused), "test.M", "fuse");
    auto k = findMethod(module->getStructures()["test.M"], "mad");
    if (!k) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_fuse_spirv", ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(k, dev)) return {};
    return print(dev);
}

std::unique_ptr<llvm::Module> spirvModule(bool unfused, llvm::LLVMContext& ctx,
                                          llvm::TargetMachine& tm,
                                          cajeta::Compiler& compiler) {
    using namespace cajeta::xpu::vulkan;
    auto module = compileForInspection(compiler, source(unfused), "test.M", "fuse");
    auto k = findMethod(module->getStructures()["test.M"], "mad");
    if (!k) return nullptr;
    auto dev = std::make_unique<llvm::Module>("xpu_fuse_spirv", ctx);
    configureDeviceModule(*dev, tm);
    if (!lowerKernel(k, *dev)) return nullptr;
    return dev;
}

std::string spirvText(bool unfused) {
    using namespace cajeta::xpu::vulkan;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) return {};
    cajeta::Compiler compiler;
    llvm::LLVMContext ctx;
    auto dev = spirvModule(unfused, ctx, *tm, compiler);
    if (!dev) return {};
    return emitSpirvText(*dev, *tm);
}

// The NoContraction decorations (Decoration 42) in a SPIR-V binary, and
// whether any UserSemantic (5635) decoration is left in it.
struct Decos { int noContraction = 0; bool userSemantic = false; bool ok = false; };

Decos spirvBinaryDecos(bool unfused) {
    using namespace cajeta::xpu::vulkan;
    Decos d;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) return d;
    cajeta::Compiler compiler;
    llvm::LLVMContext ctx;
    auto dev = spirvModule(unfused, ctx, *tm, compiler);
    if (!dev) return d;
    std::vector<uint8_t> bytes = emitSpirv(*dev, *tm);
    if (bytes.size() < 20 || bytes.size() % 4) return d;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), bytes.size());
    for (size_t i = 5; i < w.size();) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0 || i + wc > w.size()) return d;
        if ((op == 71 || op == 5632) && wc >= 3) {
            if (w[i + 2] == 42) ++d.noContraction;
            if (w[i + 2] == 5635) d.userSemantic = true;
        }
        i += wc;
    }
    d.ok = true;
    return d;
}

} // namespace

TEST(XpuFuseByDefaultTests, nvptxFusesAMultiplyAdd) {
    Lowered l = lowerForNvptx(source(false), "mad", "test.M", "sm_89", "fuse");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ir.find("fmul contract"), std::string::npos) << l.ir;
    EXPECT_NE(l.ptx.find("fma.rn.f32"), std::string::npos) << l.ptx;
}

TEST(XpuFuseByDefaultTests, unfusedKeepsTheMultiplyAndTheAddApartOnNvptx) {
    Lowered l = lowerForNvptx(source(true), "mad", "test.M", "sm_89", "fuse");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ir.find("contract"), std::string::npos) << l.ir;
    EXPECT_EQ(l.ptx.find("fma.rn.f32"), std::string::npos) << l.ptx;
}

TEST(XpuFuseByDefaultTests, amdgpuMarksItsFloatArithmeticContract) {
    Lowered l = lowerForAmdgpu(source(false), "mad", "test.M", "gfx1151", "fuse");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ir.find("fmul contract"), std::string::npos) << l.ir;
}

TEST(XpuFuseByDefaultTests, spirvMarksItsFloatArithmeticContract) {
    const std::string ir = spirvIr(false);
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("fmul contract"), std::string::npos) << ir;
}

TEST(XpuFuseByDefaultTests, unfusedDecoratesItsSpirvArithmeticNoContraction) {
    const std::string text = spirvText(true);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("NoContraction"), std::string::npos) << text;
    EXPECT_EQ(text.find("UserSemantic"), std::string::npos) << text;
    Decos d = spirvBinaryDecos(true);
    ASSERT_TRUE(d.ok);
    EXPECT_GE(d.noContraction, 2);   // the multiply and the add
    EXPECT_FALSE(d.userSemantic);
}

TEST(XpuFuseByDefaultTests, aFusedKernelLeavesItsSpirvToTheDriver) {
    const std::string text = spirvText(false);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.find("NoContraction"), std::string::npos) << text;
    Decos d = spirvBinaryDecos(false);
    ASSERT_TRUE(d.ok);
    EXPECT_EQ(d.noContraction, 0);
}

TEST(XpuFuseByDefaultTests, cpuNeverFuses) {
    const std::string ir = cpuIr(false);
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(ir.find("contract"), std::string::npos) << ir;
    EXPECT_NE(ir.find("fmul float"), std::string::npos) << ir;
}

// @Unfused on a class covers every kernel and @Device helper it declares, so a
// library whose kernels are checked bit for bit against a host oracle (cajeta-llm's
// quant mat-vecs) opts out once per class rather than once per method. A helper
// follows its own class, not its caller's.
TEST(XpuFuseByDefaultTests, unfusedOnAClassKeepsItsKernelsApart) {
    const std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "@Unfused\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void mad(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
        "                           KernelBuffer<float32> b, KernelBuffer<float32> c, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { out[i] = a[i] * b[i] + c[i]; }\n"
        "    }\n"
        "}\n";
    Lowered l = lowerForNvptx(src, "mad", "test.M", "sm_89", "fuse");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ir.find("contract"), std::string::npos) << l.ir;
    EXPECT_EQ(l.ptx.find("fma.rn.f32"), std::string::npos) << l.ptx;
}

TEST(XpuFuseByDefaultTests, aHelperInAnUnfusedClassStaysApartUnderAFusedCaller) {
    const std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "@Unfused\n"
        "class H {\n"
        "    @Device\n"
        "    static float32 mad(float32 a, float32 b, float32 c) { return a * b + c; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void two(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
        "                           KernelBuffer<float32> b, KernelBuffer<float32> c, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { out[i] = H.mad(a[i], b[i], c[i]) + a[i] * c[i]; }\n"
        "    }\n"
        "}\n";
    Lowered l = lowerForNvptx(src, "two", "test.M", "sm_89", "fuse");
    ASSERT_TRUE(l.ok) << l.why;
    // The kernel's own product-plus-sum fuses; the helper's multiply does not.
    EXPECT_NE(l.ir.find("fmul contract"), std::string::npos) << l.ir;
    EXPECT_NE(l.ir.find("fmul float"), std::string::npos) << l.ir;
}
