// Tile + mac — the cooperative multiply-accumulate of the cajeta.xpu
// cooperative-tile surface (xpu-cooperative-tile §4, Phase A Unit 3). The int8
// tier (§4.2): group.mac(acc, a, b) over int8 tiles lowers to the target's SIMD
// integer dot (dp4a — v_dot4/sdot4 on AMD), the coopQ8 path, and the author
// names no dotSum (§4.1). It routes through the SAME integerDot4x8 seam as
// Vector.dotSum (already ISA-verified by XpuVectorDeviceTests.integerDotEmits
// AmdDot4), so the ISA is confirmed by construction; the emit test here guards
// against mac diverging, and the device/CPU tests prove it computes correctly.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_macdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "Mac.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_macdev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "Mac.cajeta";
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

// Group.mac over two int8 tiles into an int32 accumulator; run() returns the
// mac result (a[i]*b[i] summed = 16 for this data). Single group (grid 1,
// block 1) — mac is a per-lane op, so the value is correct at any launch width.
const char* kMacSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Group;\n"
    "public class Mac {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out, KernelBuffer<int8> a,\n"
    "                         KernelBuffer<int8> b) {\n"
    "        Vector<int8,16> av = a.vload<16>(0);\n"
    "        Vector<int8,16> bv = b.vload<16>(0);\n"
    "        int32 acc = 0;\n"
    "        acc = Group.mac(acc, av, bv);\n"
    "        out[0] = acc;\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int8[] ha = heap int8[16];\n"
    "        int8[] hb = heap int8[16];\n"
    "        int32 ref = 0;\n"
    "        for (int32 i = 0; i < 16; i = i + 1) {\n"
    "            int8 av = (int8)(i - 8);\n"
    "            int8 bv = (int8)((i % 5) - 2);\n"
    "            ha[i] = av;\n"
    "            hb[i] = bv;\n"
    "            ref = ref + (int32) av * (int32) bv;\n"
    "        }\n"
    "        KernelBuffer<int8> da = heap KernelBuffer<int8>(0, 16);\n"
    "        KernelBuffer<int8> db = heap KernelBuffer<int8>(0, 16);\n"
    "        KernelBuffer<int32> dout = heap KernelBuffer<int32>(0, 1);\n"
    "        da.allocate();\n"
    "        db.allocate();\n"
    "        dout.allocate();\n"
    "        da.upload(ha);\n"
    "        db.upload(hb);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(dout, da, db);\n"
    "        s.sync();\n"
    "        int32[] hout = heap int32[1];\n"
    "        dout.download(hout);\n"
    "        da.free();\n"
    "        db.free();\n"
    "        dout.free();\n"
    "        return hout[0];\n"
    "    }\n"
    "}\n";

} // namespace

// 3.1.1 (AMD device): the int8 mac dp4a matches the scalar reference.
TEST(XpuTileMacDevice, int8MacEqualsScalarOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kMacSource, "test.Mac", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 16) << "Group.mac AMD result (expected 16)";
}

// 3.1.1 (CPU): the same source, via the portable integer-dot path.
TEST(XpuTileMacDevice, int8MacEqualsScalarOnCpu) {
    auto jit = CajetaJit::compile(kMacSource, "test.Mac", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 16) << "Group.mac CPU result (expected 16)";
}

// 3.1.1 (ISA): the int8 mac reaches the AMD dp4a intrinsic (amdgcn.sdot4) on
// gfx1151 — the author named `mac`, and the compiler chose the hardware dot.
// Emit-only (no device), mirroring XpuVectorDeviceTests.integerDotEmitsAmdDot4.
TEST(XpuTileMacDevice, int8MacEmitsAmdDot4) {
    using namespace cajeta::xpu::amd;
    Compiler compiler;
    auto module = compileForInspection(compiler, kMacSource);
    auto k = findMethod(module->getStructures()["test.Mac"], "k");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_mac_amd_emit", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(k, deviceModule), nullptr);

    std::string ir;
    { llvm::raw_string_ostream os(ir); deviceModule.print(os, nullptr); }
    EXPECT_NE(ir.find("amdgcn.sdot4"), std::string::npos)
        << "Group.mac int8 must reach the gfx1151 dot4 unit (the author named "
           "mac, not dotSum)";
}
