// cajeta-ml-gpu 23.2.6a: bfloat16 reaches SPIR-V as 16-bit integers, widened explicitly, so a bf16 kernel needs
// no VK_KHR_shader_bfloat16. CAJETA_TEST_SPV_OUT=<prefix> writes each module's SPIR-V.
#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;
using cajeta_test::CajetaJit;

namespace {

// bf16 through buffer loads and stores, a shared tile, a 4-wide load, a loop-carried value, a select,
// widening into float32 arithmetic and the round-to-nearest-even narrowing.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class B {\n"
    "    @Kernel\n"
    "    public static void mix(KernelBuffer<bfloat16> out, KernelBuffer<bfloat16> a,\n"
    "                           KernelBuffer<float32> f, uint32 n) {\n"
    "        Shared<bfloat16> sh = shared bfloat16[64];\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 t = KernelThread.x();\n"
    "        sh[t] = a[i];\n"
    "        Barrier.workgroup();\n"
    "        Vector<bfloat16,4> v = a.vload<4>((i / 4) * 4);\n"
    "        bfloat16 best = sh[(t + 1) % 64];\n"
    "        uint32 j = 0;\n"
    "        while (j < 4) {\n"
    "            bfloat16 c = a[(i + j) % n];\n"
    "            best = (float32) c > (float32) best ? c : best;\n"
    "            j = j + 1;\n"
    "        }\n"
    "        float32 s = (float32) best + (float32) v.y * f[i];\n"
    "        if (i < n) { out[i] = (bfloat16) s; }\n"
    "    }\n"
    "    /** Outputs of `mix` on 64 inputs, as float32 bits summed with their index: a fingerprint to compare. */\n"
    "    public static int64 run() {\n"
    "        float32[] ha = heap float32[64];\n"
    "        float32[] hf = heap float32[64];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) {\n"
    "            ha[i] = (float32) ((i * 37) % 61) * 0.37f - 9.0f;\n"
    "            hf[i] = 1.0f + (float32) (i % 5) * 0.125f;\n"
    "            i = i + 1;\n"
    "        }\n"
    "        bfloat16[] hb = heap bfloat16[64];\n"
    "        i = 0;\n"
    "        while (i < 64) { hb[i] = (bfloat16) ha[i]; i = i + 1; }\n"
    "        KernelBuffer<bfloat16> a = heap KernelBuffer<bfloat16>(64);\n"
    "        KernelBuffer<bfloat16> out = heap KernelBuffer<bfloat16>(64);\n"
    "        KernelBuffer<float32> f = heap KernelBuffer<float32>(64);\n"
    "        a.upload(hb);\n"
    "        f.upload(hf);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        mix.launch(s, grid: [1], block: [64])(out, a, f, (uint32) 64);\n"
    "        s.sync();\n"
    "        bfloat16[] got = heap bfloat16[64];\n"
    "        out.download(got);\n"
    "        int64 sum = 0;\n"
    "        i = 0;\n"
    "        while (i < 64) {\n"
    "            sum = sum + (int64) ((float32) got[i] * 64.0f) * (int64) (i + 1);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

int64_t fingerprintOn(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.B", o);
    if (!jit) return INT64_MIN;
    auto run = jit->lookup<int64_t (*)()>("run");
    if (!run) return INT64_MIN + 1;
    return run();
}

bool contains(const std::vector<uint8_t>& bytes, const std::string& s) {
    return std::search(bytes.begin(), bytes.end(), s.begin(), s.end()) != bytes.end();
}


// The device module's IR after emitSpirv for `method` of `cls`, its SPIR-V in `spirv`; empty on failure.
std::string emittedIr(const char* source, const char* cls, const char* method, const char* tag,
                      std::vector<uint8_t>& spirv) {
    using namespace cajeta::xpu::vulkan;
    auto tm = createSpirvTargetMachine("vulkan1.3");
    if (!tm) return "";
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source, cls, tag);
    auto k = findMethod(module->getStructures()[cls], method);
    if (!k) return "";
    llvm::LLVMContext ctx;
    llvm::Module dev(std::string("xpu_") + tag, ctx);
    configureDeviceModule(dev, *tm);
    if (lowerKernel(k, dev) == nullptr) return "";
    std::string failure;
    spirv = emitSpirv(dev, *tm, &failure);
    if (spirv.empty()) return "";
    if (const char* out = std::getenv("CAJETA_TEST_SPV_OUT")) {
        std::ofstream fo(std::string(out) + "." + tag + ".spv", std::ios::binary);
        fo.write(reinterpret_cast<const char*>(spirv.data()), (std::streamsize) spirv.size());
    }
    std::string ir;
    llvm::raw_string_ostream os(ir);
    dev.print(os, nullptr);
    os.flush();
    return ir;
}

// The software cooperative-matrix tile at bf16, all three tiles bf16 (XpuCooperativeMatrixDeviceTests' CM6).
const char* kCoopSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bmatmul(KernelBuffer<bfloat16> a, KernelBuffer<bfloat16> b,\n"
    "                               KernelBuffer<bfloat16> c) {\n"
    "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<bfloat16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<bfloat16,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

} // namespace

// 23.2.6a: no bfloat type reaches codegen and the module declares no bf16 extension.
TEST(SpirvBf16StorageTests, aBf16KernelReachesSpirvAsSixteenBitIntegers) {
    std::vector<uint8_t> spirv;
    std::string ir = emittedIr(kSource, "test.B", "mix", "bf16store", spirv);
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(ir.find("bfloat"), std::string::npos) << "a bfloat type reached SPIR-V codegen:\n" << ir;
    EXPECT_FALSE(contains(spirv, "SPV_KHR_bfloat16")) << "the module declares SPV_KHR_bfloat16";
}

// 23.2.6a: the bf16 software cooperative-matrix tile lowers the same way.
TEST(SpirvBf16StorageTests, theBf16SoftwareCoopTileReachesSpirvAsSixteenBitIntegers) {
    std::vector<uint8_t> spirv;
    std::string ir = emittedIr(kCoopSource, "test.M", "bmatmul", "bf16coop", spirv);
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(ir.find("bfloat"), std::string::npos) << "a bfloat type reached SPIR-V codegen:\n" << ir;
    EXPECT_FALSE(contains(spirv, "SPV_KHR_bfloat16")) << "the module declares SPV_KHR_bfloat16";
}

// 23.2.6a: the integer form computes what the cpu backend does, bit for bit.
TEST(SpirvBf16StorageTests, vulkanAgreesWithTheCpuBackendOnEveryBf16Shape) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    int64_t cpu = fingerprintOn(cajeta::xpu::Backend::Cpu);
    ASSERT_GT(cpu, INT64_MIN + 1);
    EXPECT_EQ(fingerprintOn(cajeta::xpu::Backend::Spirv), cpu);
}
