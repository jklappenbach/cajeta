//
// A workgroup tile bigger than NVIDIA's STATIC shared cap.
//
// THE FAILURE. Three cajeta-llm kernels assembled to nothing on the 4090,
// with no `[xpu-kernel-skipped]` note to explain it because they are not
// skips — ptxas rejects them:
//
//   ptxas error: Entry function 'q4kF16CoopN256Kernel' uses too much shared
//                data (0xd800 bytes, 0xc000 max)
//
// 0xd800 is 55296 and 0xc000 is 49152. `q4kF16CoopN256Kernel` stages
// `float16[128*72]` plus `float16[256*72]` — 18432 + 36864 bytes — and that
// shape is not arbitrary: it was measured on AMD, where a workgroup gets
// 64 KB of LDS, and doubling the token tile to 256 bought 7.70 -> 8.16
// TMAC/s by halving the A-side staging. `attnFlashPrefillTileKernel` wants
// 62016 bytes for the same reason.
//
// WHY IT IS A COMPILER PROBLEM AND NOT A KERNEL PROBLEM. 48 KB is the cap on
// STATIC `.shared` in the PTX ABI, on every sm_*. It is not the cap on shared
// memory: the same sm_89 will give a block up to 99 KB through the DYNAMIC
// path — one `extern .shared` block, sized at launch, opted into with
// cuFuncSetAttribute. So the hardware has the memory, and the only thing
// standing between the kernel and it is which PTX declaration the compiler
// chose. Making the author pick per backend would be exactly the per-device
// maintenance the whole xpu design exists to avoid, and shrinking the tile
// to 48 KB would give NVIDIA a slower kernel to spare the compiler a pass.
//
// WHAT THIS PINS. Over the cap, the static globals are relocated into one
// extern block at fixed offsets and the byte count is recorded for the
// launch. Under it, nothing changes — the static declaration is better (no
// launch parameter, no opt-in call, and ptxas can see the size), so the
// relocation must not fire when it is not needed.
//
// The assemble is the real assertion. A PTX-text check alone would pass for
// a rewrite that produced the right directives and the wrong offsets; ptxas
// accepting the module is what says the shape is legal, and the device test
// in XpuNvptxBigSharedDeviceTests is what says the offsets are right.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_bigsh_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_bigsh_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(),
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

// `big` stages 18432 + 36864 = 55296 bytes, q4kF16CoopN256Kernel's exact
// shape. `small` stages 2048, comfortably under. Both touch every element so
// nothing is dead-stripped before it reaches ptxas.
const char* kSrc =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void big(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float16> sa = shared float16[128 * 72];\n"
    "        Shared<float16> sb = shared float16[256 * 72];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = t;\n"
    "        while (i < 9216) { sa[i] = (float16) in[(int64) (i % n)];\n"
    "                           i = i + 256; }\n"
    "        uint32 j = t;\n"
    "        while (j < 18432) { sb[j] = (float16) in[(int64) (j % n)];\n"
    "                            j = j + 256; }\n"
    "        Barrier.workgroup();\n"
    "        float32 acc = 0.0f;\n"
    "        uint32 k = t;\n"
    "        while (k < 9216) { acc = acc + (float32) sa[k] "
                                "+ (float32) sb[k + 9216];\n"
    "                           k = k + 256; }\n"
    "        out[(int64) t] = acc;\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void small(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> s = shared float32[512];\n"
    "        uint32 t = KernelThread.x();\n"
    "        s[t] = in[(int64) (t % n)];\n"
    "        Barrier.workgroup();\n"
    "        out[(int64) t] = s[t] + s[511 - t];\n"
    "    }\n"
    "}\n";

std::string ptxFor(Compiler& compiler, CajetaModulePtr module,
                   const char* kernelName) {
    auto k = findMethod(module->getStructures()["test.M"], kernelName);
    EXPECT_NE(k, nullptr);
    if (!k) return {};
    auto tm = createNvptxTargetMachine("sm_89");
    EXPECT_NE(tm, nullptr);
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module dev(std::string("xpu.bigsh.") + kernelName, ctx);
    configureDeviceModule(dev, *tm);
    if (!lowerKernel(k, dev)) return {};
    return emitPtx(dev, *tm);
}

// Every `.shared` byte ptxas will count as STATIC: the sizes of the
// `.shared .align N .b8 name[BYTES]` definitions, which excludes the
// `.extern .shared` declaration (that one carries no size).
unsigned staticSharedBytes(const std::string& ptx) {
    unsigned total = 0;
    size_t at = 0;
    while ((at = ptx.find(".shared", at)) != std::string::npos) {
        size_t lineStart = ptx.rfind('\n', at);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::string line = ptx.substr(lineStart, ptx.find('\n', at) - lineStart);
        at += 7;
        if (line.find(".extern") != std::string::npos) continue;
        size_t lb = line.find('[');
        size_t rb = line.find(']', lb == std::string::npos ? 0 : lb);
        if (lb == std::string::npos || rb == std::string::npos) continue;
        std::string num = line.substr(lb + 1, rb - lb - 1);
        if (num.empty()) continue;
        try { total += (unsigned) std::stoul(num); } catch (...) { }
    }
    return total;
}

} // namespace

TEST(XpuNvptxBigShared, aTileOverTheStaticCapMovesToTheExternBlock) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSrc);
    std::string ptx = ptxFor(compiler, module, "big");
    ASSERT_FALSE(ptx.empty());

    EXPECT_NE(ptx.find(".extern .shared"), std::string::npos)
        << "the relocated tile needs one extern shared block:\n" << ptx;
    EXPECT_LE(staticSharedBytes(ptx), 49152u)
        << "nothing over the static cap may be left as a static definition";

    if (findPtxas().empty())
        GTEST_SKIP() << "the PTX half above passed; assembling it needs ptxas, "
                        "which is not on this box (CUDA_PATH or PATH)";
    std::string log;
    auto cubin = assembleCubin(ptx, "sm_89", &log);
    EXPECT_FALSE(cubin.empty())
        << "ptxas must accept the relocated module:\n" << log;
    EXPECT_EQ(log.find("too much shared data"), std::string::npos) << log;
}

TEST(XpuNvptxBigShared, aTileUnderTheCapKeepsItsStaticDeclaration) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSrc);
    std::string ptx = ptxFor(compiler, module, "small");
    ASSERT_FALSE(ptx.empty());

    EXPECT_EQ(ptx.find(".extern .shared"), std::string::npos)
        << "a tile that fits must stay static — the extern block costs a "
           "launch parameter and an opt-in call:\n" << ptx;
    EXPECT_EQ(staticSharedBytes(ptx), 2048u) << ptx;
}
