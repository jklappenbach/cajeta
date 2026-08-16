//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 4) — SAXPY on a real AMD
// GPU (gfx1151 / Strix Halo), end to end.
//
// The AMD analog of XpuSaxpyDeviceTests. The whole AMDGPU pipeline on the GPU:
//   Cajeta @Kernel source
//     -> AmdgpuKernelLowering (shared AST walk + AMDGPU LoweringTarget)
//     -> AmdgpuBackend::emitIsa / assembleHsaco (LLVM -> object -> lld -> hsaco)
//     -> HipDriver (hipModuleLoadData / hipModuleLaunchKernel)
//     -> verify y[i] == a*x[i] + y[i].
//
// This is the variance-discipline payoff: the SAME SAXPY source that runs on
// NVIDIA runs here, with only LoweringTarget + the driver forking. Skips
// cleanly when no ROCm/HIP device is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Program.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_amddev_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_amddev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
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

} // namespace


// Item 4: a MULTI-ARCH bundle (gfx1100 + gfx1151) built by assembleHsacoBundle
// loads via hipModuleLoadData — the HIP runtime selects the running device's arch
// — and the kernel runs correctly on-device. Proves AMD multi-arch fatbin end to
// end (bundle → load → select → launch). Skips without HIP / the bundler.

// ----- Stage 11: bounded device-side dispatch on a real AMD device -----
//
// An indexed dispatch table over three @Device statics, lowered to AMDGPU ISA:
// `((int32)->int32)[] ops = { sq, cube, neg }` is an i32 tag over a closed
// candidate set; `ops[i % 3](i)` lowers to an if/else chain of DIRECT calls (no
// function pointers). i%3==0 -> i*i, ==1 -> i*i*i, ==2 -> -i. The index is a
// runtime value, so a constant-folded single candidate would fail. n kept small
// so cube(i) stays in int32 range.
TEST(XpuSaxpyAmdDeviceTests, deviceDispatchTableOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class Ops {\n"
        "    @Device public static int32 sq(int32 x)   { return x * x; }\n"
        "    @Device public static int32 cube(int32 x) { return x * x * x; }\n"
        "    @Device public static int32 neg(int32 x)  { return 0 - x; }\n"
        "}\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void dispatch(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            ((int32) -> int32)[] ops = { Ops::sq, Ops::cube, Ops::neg };\n"
        "            uint32 sel = i % 3;\n"
        "            out[i] = ops[sel]((int32) i);\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto dispatch = findMethod(module->getStructures()["test.M"], "dispatch");
    ASSERT_NE(dispatch, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_dispatch_amddevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(dispatch, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    const unsigned n = 96;            // multiple of 3; cube(95) in int32 range
    std::vector<int32_t> out(n, -999);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "dispatch");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(int32_t);
    HipDevicePtr dOut = hip.alloc(bytes);
    ASSERT_NE(dOut, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), bytes));

    void* params[] = { &dOut, (void*) &n };
    const unsigned block = 64;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(hip.launch(fn, grid, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<int32_t> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, bytes));
    hip.free(dOut);

    size_t mismatches = 0;
    for (unsigned i = 0; i < n; ++i) {
        int32_t want;
        switch (i % 3) {
            case 0:  want = (int32_t)(i * i);         break;
            case 1:  want = (int32_t)(i * i * i);      break;
            default: want = -(int32_t) i;             break;
        }
        if (result[i] != want) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << want;
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}
