//
// Constant expressions over a class template's values
// (xpu-kernel-independence plan Unit 5, spec §3.3; moved from
// xpu-tile-shape-selection Unit 3).
//
// A tile written once over its shape derives its bounds from that shape: the
// workgroup's wave count in `@Occupancy`, and the size of its fragment
// arrays. Both are integer constant expressions over the class's non-type
// parameters, evaluated per instantiation. One that is not a compile-time
// constant is refused by name, never read as zero or as text.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "KernelLoweringProbe.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <string>

using cajeta_test::CajetaJit;
using namespace cajeta::xpu::probe;

namespace {

const char* kHead = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
)CJ";

// The shape's waves: (TM / WM) * (TN / WN), 8 for 128x128 over 32x64 warp
// tiles and 16 over 32x32.
const char* kOccupancy = R"CJ(
public class Q<uint32 TM, uint32 TN, uint32 WM, uint32 WN> {
    public Q() { return; }
    @Kernel
    @Occupancy(maxWaves = (TM / WM) * (TN / WN))
    public static void k(KernelBuffer<uint32> out) {
        out[KernelThread.globalIdX()] = TM;
    }
    public void run(KernelBuffer<uint32> out) {
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [256])(out);
        s.sync();
    }
}
public class M {
    public static int32 run() {
        KernelBuffer<uint32> b = heap KernelBuffer<uint32>(0, 512);
        b.allocate();
        Q<128, 128, 32, 64> a #= heap Q<128, 128, 32, 64>();
        a.run(b);
        Q<128, 128, 32, 32> c #= heap Q<128, 128, 32, 32>();
        c.run(b);
        return 0;
    }
}
)CJ";

// A fragment array sized by the warp tile: 2x4 fragments for a 32x64 warp
// tile, 2x2 for 32x32. Each fragment stores a 16x16 block of ones, so the
// sum of the output over 256 is the fragment count.
const char* kFragments = R"CJ(
public class F<uint32 WM, uint32 WN> {
    public F() { return; }
    @Kernel
    public static void k(KernelBuffer<float32> c) {
        CooperativeMatrix<float32,16,16,2>[WM / 16][WN / 16] acc;
        for (uint32 i = 0; i < WM / 16; i = i + 1) {
            for (uint32 j = 0; j < WN / 16; j = j + 1) {
                acc[i][j].splat(1.0f);
                acc[i][j].store(c, (i * (WN / 16) + j) * 256, 0, 16);
            }
        }
    }
    public int32 fragments() {
        KernelBuffer<float32> c = heap KernelBuffer<float32>(0, 4096);
        c.allocate();
        float32[] h = heap float32[4096];
        int32 z = 0;
        while (z < 4096) { h[z] = 0.0f; z = z + 1; }
        c.upload(h);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [32])(c);
        s.sync();
        c.download(h);
        c.free();
        float32 sum = 0.0f;
        z = 0;
        while (z < 4096) { sum = sum + h[z]; z = z + 1; }
        return (int32) sum / 256;
    }
}
public class M {
    public static int32 run() {
        F<32, 64> a #= heap F<32, 64>();
        F<32, 32> b #= heap F<32, 32>();
        return a.fragments() * 10 + b.fragments();
    }
}
)CJ";

// The kernel `k` of each instantiation of `cls`, by the instantiation's
// argument text, lowered for sm_89.
std::string ptxFor(const std::string& source, const std::string& cls,
                   const std::string& args, std::string* why) {
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, source);
    if (!module) { *why = "did not compile"; return ""; }
    cajeta::MethodPtr k;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon.rfind("test." + cls + "<", 0) != 0 || !klass) continue;
        std::string a = canon.substr(canon.find('<'));
        std::string flat;
        for (char ch : a) if (ch != ' ') flat += ch;
        if (flat != "<" + args + ">") continue;
        k = findMethod(klass, "k");
    }
    if (!k) { *why = "no instantiation " + cls + "<" + args + ">"; return ""; }
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    if (!tm) { *why = "no sm_89 target machine"; return ""; }
    llvm::LLVMContext ctx;
    llvm::Module dev("probe_dev", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(dev, *tm);
    try {
        cajeta::xpu::nvidia::lowerKernel(k, dev);
    } catch (cajeta::Exception& e) {
        *why = e.getMessage();
        return "";
    }
    return cajeta::xpu::nvidia::emitPtx(dev, *tm);
}

int runOn(const std::string& source, cajeta::xpu::Backend backend) {
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(source, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -2;
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -2;
}

} // namespace

// 4.5.1.1: the bound travels with the shape.
TEST(XpuTemplateConstExpr, occupancyIsAnExpressionOverTheShape) {
    const std::string src = std::string(kHead) + kOccupancy;
    std::string why;
    const std::string eight = ptxFor(src, "Q", "128,128,32,64", &why);
    ASSERT_FALSE(eight.empty()) << why;
    EXPECT_NE(eight.find(".maxntid 256"), std::string::npos) << eight.substr(0, 2000);
    const std::string sixteen = ptxFor(src, "Q", "128,128,32,32", &why);
    ASSERT_FALSE(sixteen.empty()) << why;
    EXPECT_NE(sixteen.find(".maxntid 512"), std::string::npos)
        << sixteen.substr(0, 2000);
}

// 4.5.1.2: a name that is not a template value is refused, naming the
// annotation, the argument and the name.
TEST(XpuTemplateConstExpr, aNonConstantExpressionIsRefusedByName) {
    std::string src = std::string(kHead) + kOccupancy;
    const std::string from = "maxWaves = (TM / WM) * (TN / WN)";
    src.replace(src.find(from), from.size(), "maxWaves = TM / lanes");
    std::string why;
    const std::string ptx = ptxFor(src, "Q", "128,128,32,64", &why);
    ASSERT_TRUE(ptx.empty()) << "a non-constant @Occupancy lowered";
    EXPECT_NE(why.find("@Occupancy"), std::string::npos) << why;
    EXPECT_NE(why.find("maxWaves"), std::string::npos) << why;
    EXPECT_NE(why.find("'lanes'"), std::string::npos) << why;
}

// 4.5.1.3: a fragment array's sizes are expressions over the warp tile, and
// each instantiation gets its own.
TEST(XpuTemplateConstExpr, aFragmentArrayIsSizedPerInstantiationOnCpu) {
    EXPECT_EQ(runOn(std::string(kHead) + kFragments, cajeta::xpu::Backend::Cpu), 84)
        << "F<32,64> holds 8 fragments and F<32,32> holds 4";
}

TEST(XpuTemplateConstExpr, aFragmentArrayIsSizedPerInstantiationOnNvptx) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runOn(std::string(kHead) + kFragments, cajeta::xpu::Backend::Nvptx), 84)
        << "F<32,64> holds 8 fragments and F<32,32> holds 4";
}
