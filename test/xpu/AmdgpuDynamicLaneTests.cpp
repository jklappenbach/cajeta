//
// xpu-kernel-adaptor 1.5.5.7.4: a runtime-indexed lane read, measured on AMDGPU
// before touching its legalization. NVPTX legalized `v[k]` through a stack frame
// and needed a select chain; AMDGPU keeps the plain extractelement, and these
// tests pin what that costs on gfx1151 (no scratch) and that it reads the lane
// it names on the device.
//

#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"

#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuRegistration.h"
#include "cajeta/xpu/core/KernelManifest.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;
using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dyn(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 b = (int64) i * 8L;\n"
    "            Vector<float32,8> v = xs.vload<8>(b) * 0.0f;\n"
    "            float32 d = xs[b];\n"
    "            v[0] = d * 1.0f;\n"
    "            v[1] = d * 2.0f;\n"
    "            v[2] = d * 3.0f;\n"
    "            v[3] = d * 4.0f;\n"
    "            v[4] = d * 5.0f;\n"
    "            v[5] = d * 6.0f;\n"
    "            v[6] = d * 7.0f;\n"
    "            v[7] = d * 8.0f;\n"
    "            int32 k = sel[(int64) i];\n"
    "            out[(int64) i] = v[k];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 256;\n"
    "        float32[] hx = heap float32[2048];\n"
    "        int32[] hs = heap int32[256];\n"
    "        int32 i = 0;\n"
    "        while (i < 2048) { hx[i] = 0.5f + (float32) (i % 37); i = i + 1; }\n"
    "        i = 0;\n"
    "        while (i < 256) { hs[i] = (i * 5 + 3) % 8; i = i + 1; }\n"
    "        KernelBuffer<float32> xs = heap KernelBuffer<float32>(2048);\n"
    "        KernelBuffer<int32> sel = heap KernelBuffer<int32>(256);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(256);\n"
    "        xs.upload(hx);\n"
    "        sel.upload(hs);\n"
    "        float32[] ho = heap float32[256];\n"
    "        i = 0;\n"
    "        while (i < 256) { ho[i] = -1.0f; i = i + 1; }\n"
    "        out.upload(ho);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        dyn.launch(s, grid: [4], block: [64])(out, xs, sel, n);\n"
    "        s.sync();\n"
    "        out.download(ho);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 256) {\n"
    "            float32 want = hx[i * 8] * (float32) (hs[i] + 1);\n"
    "            if (ho[i] != want) { bad = bad + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

// The runtime index costs no scratch on gfx1151: no private segment and no
// scratch instruction in the ISA.
TEST(AmdgpuDynamicLaneTests, dynamicLaneLeavesNoScratchOnAmdgpu) {
    CAJETA_SKIP_IF_NO_HIP();
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource);
    auto k = findMethod(module->getStructures()["test.M"], "dyn");
    ASSERT_NE(k, nullptr);

    llvm::LLVMContext ctx;
    llvm::Module host("xpu_dynlane_amdgpu", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx1151", {}, &out);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(out.size(), 1u) << err;
    ASSERT_TRUE(out[0].spillBytes.has_value());
    EXPECT_EQ(*out[0].spillBytes, 0u) << cajeta::xpu::toJson(out[0]);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext dctx;
    llvm::Module dev("xpu_dynlane_amdgpu_isa", dctx);
    cajeta::xpu::amd::configureDeviceModule(dev, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, dev), nullptr);
    std::string isa = cajeta::xpu::amd::emitIsa(dev, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_EQ(isa.find("scratch_"), std::string::npos) << isa;
}

// The lane read on the device names the lane it asks for.
TEST(AmdgpuDynamicLaneTests, dynamicLaneMatchesTheLaneItNamesOnDevice) {
    CAJETA_SKIP_IF_NO_HIP();
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(jit) << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0) << err;
}
