//
// CajetaXPU — AsyncCopy native AMDGPU lowering (xpu-pipelined-gemm-primitives
// Unit 2).
//
// KEY HARDWARE FINDING: the direct global->LDS load `global_load_lds`
// (LLVM's VMemToLDSLoad) is a CDNA/GFX9 + gfx1250 feature — RDNA1-3.5
// (gfx10xx/gfx11xx, incl. THIS box's gfx1151) does NOT have it; emitting it there
// Cannot-selects. So AsyncCopy.copy is arch-gated: native `global_load_lds` on a
// subtarget that has it (CDNA), and the synchronous staged copy otherwise (RDNA).
// `commit` is a no-op (no gfx1250 async-mark); `wait` drains vmem (a workgroup
// AcqRel fence -> s_waitcnt) so the caller's Barrier publishes the landed tile.
//
// Three checks:
//   (1) on-device (gfx1151) — an AsyncCopy-staged tile read cross-lane after a
//       barrier is correct for every work-item (the sync-fallback staging landed);
//   (2) ISA on a CDNA arch (gfx942, GPU-free) — the native path emits
//       `global_load_lds` (the LDS-direct, no-register-round-trip path);
//   (3) ISA on gfx1151 (GPU-free) — compiles cleanly via the sync fallback (no
//       `global_load_lds`, no Cannot-select).
//
// The device run skips cleanly when no ROCm/HIP device is present; the ISA checks
// are GPU-free.
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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Stage in[0..256) into an LDS tile via AsyncCopy (whole-panel, workgroup-strided),
// commit + wait(0) to drain, barrier, then read a DIFFERENT lane (tile[255-t]).
// out[t] == in[255-t] == 255-t for every t proves the global_load_lds writes
// landed for the whole block before any cross-lane read.
const char* kAsyncStageSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.AsyncCopy;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void asyncstage(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Shared<uint32> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        AsyncCopy.copy(tile, 0, in, 0, 256);\n"
    "        AsyncCopy.commit();\n"
    "        AsyncCopy.wait(0);\n"
    "        Barrier.workgroup();\n"
    "        out[t] = tile[255 - t];\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_asynccopy_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_asynccopy_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
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

// U2.1/2.3 (on-device): an AsyncCopy-staged tile read cross-lane after a barrier
// is correct for every work-item on gfx1151.
TEST(XpuAsyncCopyAmdDeviceTests, asyncStagedTileRunsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kAsyncStageSrc);
    auto method = findMethod(module->getStructures()["test.M"], "asyncstage");
    ASSERT_NE(method, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_asynccopy_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(method, deviceModule), nullptr);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 256;
    std::vector<uint32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = i;

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "asyncstage");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dOut = hip.alloc(std::size_t(n) * sizeof(uint32_t));
    HipDevicePtr dIn = hip.alloc(std::size_t(n) * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr);
    ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(uint32_t)));

    void* params[] = { &dOut, &dIn };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/256, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<uint32_t> out(n, 0);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dOut, std::size_t(n) * sizeof(uint32_t)));
    hip.free(dOut);
    hip.free(dIn);

    for (uint32_t i = 0; i < n; ++i)
        EXPECT_EQ(out[i], 255u - i) << "lane " << i << " staged wrong";
}

namespace {
std::string emitIsaForArch(const char* arch) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAsyncStageSrc);
    auto method = findMethod(module->getStructures()["test.M"], "asyncstage");
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine(arch);
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_asynccopy_isa", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return emitIsa(deviceModule, *tm);
}
} // namespace

// U2.1 (ISA, GPU-free): on a CDNA arch (which has VMemToLDSLoad) AsyncCopy.copy
// lowers to the LDS-direct load `global_load_lds` — the no-register-round-trip
// path, NOT a global_load + ds_store staging.
TEST(XpuAsyncCopyAmdDeviceTests, cdnaIsaUsesGlobalLoadLds) {
    std::string isa = emitIsaForArch("gfx942");
    ASSERT_FALSE(isa.empty()) << "ISA emit failed for gfx942";
    EXPECT_NE(isa.find("global_load_lds"), std::string::npos)
        << "expected the LDS-direct load on a CDNA subtarget\n" << isa;
}

// U2.1 (ISA, GPU-free): on gfx1151 (RDNA3.5, NO VMemToLDSLoad) the kernel still
// compiles cleanly via the synchronous staged-copy fallback — no global_load_lds,
// and crucially no Cannot-select hard error.
TEST(XpuAsyncCopyAmdDeviceTests, gfx1151FallsBackToSyncStaging) {
    std::string isa = emitIsaForArch("gfx1151");
    ASSERT_FALSE(isa.empty()) << "ISA emit failed for gfx1151";
    EXPECT_EQ(isa.find("global_load_lds"), std::string::npos)
        << "gfx1151 has no direct global->LDS load; must use the sync fallback";
}
