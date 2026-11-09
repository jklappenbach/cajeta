//
// CoopBlockPadOffsetTests — a cooperative-matrix load from a BlockPadded tile
// at a nonzero offset must read at that offset, on every backend.
//
// Why this exists. For a BlockPadded tile the lowering hands the target the
// bare tile base and carries the fragment's logical offset inside the
// LdsBlockPad descriptor, so a target that pads can map base and offset
// together. NVPTX and SPIR-V take the descriptor and drop it, so every load
// from a padded tile read at the tile base whatever its offset. The f16 GEMM
// bench (bench/xpu/GpuMatMulF16Bench) came back with sum C 2.03x sum B on
// cuda and 2.5x on vulkan, measured 2026-11-08 (cajeta-ml-gpu 23.2.3). The
// BlockPadded probe tests never caught it because they load at offset 0 only.
//
// The test lowers the same kernel twice, loading at offset 0 and at 1024,
// and requires the two device functions to differ. On a backend that drops
// the offset they are identical, which is the failure. AMDGPU honored the
// descriptor all along, so it is the control that the shape can tell them
// apart.
//
#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace cajeta::xpu::probe;

namespace {

std::string source(const char* tile, unsigned offset) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.Shared;\n"
        "import cajeta.xpu.BlockPadded;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public class M {\n"
        "    @Kernel @Wave(width = 32)\n"
        "    public static void bpo(KernelBuffer<float32> c, KernelBuffer<float16> a, uint32 n) {\n"
        "        ";
    s += tile;
    s += " sa = shared float16[2304];\n"
         "        uint32 e = KernelThread.x();\n"
         "        while (e < 256) {\n"
         "            Vector<float16,8> v = a.vload<8>(e * 8);\n"
         "            sa.vstore(e * 8, v);\n"
         "            e = e + 32;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float16,16,16,0> wa; wa.load(sa, ";
    s += std::to_string(offset);
    s += ", 0, 64);\n"
         "        CooperativeMatrix<float16,16,16,1> wb; wb.load(sa, 0, 1, 64);\n"
         "        CooperativeMatrix<float32,16,16,2> acc; acc.splat(0.0f);\n"
         "        acc.mma(wa, wb);\n"
         "        acc.store(c, 0, 0, n);\n"
         "    }\n"
         "}\n";
    return s;
}

const char* kPadded = "BlockPadded<float16,64,8>";
const char* kPlain = "Shared<float16>";

// The text of the kernel's `define ... { ... }`, so module names and salted
// paths do not make two lowerings differ.
std::string kernelBody(const std::string& ir) {
    std::size_t at = ir.find("define ");
    while (at != std::string::npos && ir.find("bpo", at) > ir.find('\n', at))
        at = ir.find("define ", at + 1);
    if (at == std::string::npos) return {};
    std::size_t end = ir.find("\n}\n", at);
    return ir.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::string nvptxBody(const char* tile, unsigned offset, std::string* why) {
    Lowered l = lowerForNvptx(source(tile, offset), "bpo", "test.M", "sm_89", "bpoff");
    if (!l.ok) { *why = l.why; return {}; }
    return kernelBody(l.ir);
}

std::string amdgpuBody(const char* tile, unsigned offset, std::string* why) {
    Lowered l = lowerForAmdgpu(source(tile, offset), "bpo", "test.M", "gfx1151", "bpoff");
    if (!l.ok) { *why = l.why; return {}; }
    return kernelBody(l.ir);
}

std::string spirvBody(const char* tile, unsigned offset, std::string* why) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source(tile, offset), "test.M", "bpoff");
    auto k = findMethod(module->getStructures()["test.M"], "bpo");
    if (!k) { *why = "kernel bpo not found"; return {}; }
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) { *why = "no SPIR-V target machine"; return {}; }
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_bpoff", ctx);
    configureDeviceModule(dev, *tm);
    try {
        if (!lowerKernel(k, dev)) { *why = "lowerKernel returned null"; return {}; }
    } catch (const std::exception& ex) {
        *why = ex.what();
        return {};
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    dev.print(os, nullptr);
    os.flush();
    return kernelBody(ir);
}

} // namespace

TEST(CoopBlockPadOffsetTests, amdgpuReadsAPaddedTileAtTheGivenOffset) {
    std::string why;
    const std::string at0 = amdgpuBody(kPadded, 0, &why);
    ASSERT_FALSE(at0.empty()) << why;
    const std::string at1024 = amdgpuBody(kPadded, 1024, &why);
    ASSERT_FALSE(at1024.empty()) << why;
    EXPECT_NE(at0, at1024) << "the control lowered offset 0 and 1024 alike";
}

TEST(CoopBlockPadOffsetTests, nvptxReadsAPaddedTileAtTheGivenOffset) {
    std::string why;
    const std::string at0 = nvptxBody(kPadded, 0, &why);
    ASSERT_FALSE(at0.empty()) << why;
    const std::string at1024 = nvptxBody(kPadded, 1024, &why);
    ASSERT_FALSE(at1024.empty()) << why;
    EXPECT_NE(at0, at1024) << "nvptx dropped the fragment offset of a padded tile:\n" << at1024;
}

TEST(CoopBlockPadOffsetTests, spirvReadsAPaddedTileAtTheGivenOffset) {
    std::string why;
    const std::string at0 = spirvBody(kPadded, 0, &why);
    ASSERT_FALSE(at0.empty()) << why;
    const std::string at1024 = spirvBody(kPadded, 1024, &why);
    ASSERT_FALSE(at1024.empty()) << why;
    EXPECT_NE(at0, at1024) << "spir-v dropped the fragment offset of a padded tile:\n" << at1024;
}

// A target that does not pad lays a BlockPadded tile out linearly, so its
// coop load must address exactly as a plain Shared tile's does.
TEST(CoopBlockPadOffsetTests, nvptxAddressesAPaddedTileLikeAPlainOne) {
    std::string why;
    const std::string padded = nvptxBody(kPadded, 1024, &why);
    ASSERT_FALSE(padded.empty()) << why;
    const std::string plain = nvptxBody(kPlain, 1024, &why);
    ASSERT_FALSE(plain.empty()) << why;
    EXPECT_EQ(padded, plain);
}
