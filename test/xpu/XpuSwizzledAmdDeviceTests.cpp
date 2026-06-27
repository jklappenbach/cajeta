//
// CajetaXPU — Swizzled<T,S> native AMDGPU lowering (xpu-pipelined-gemm-primitives
// Unit 3).
//
// A Swizzled<T,S> tile permutes its element addressing with the conflict-free XOR
// `col ^= row & (S-1)` (an involution, applied identically on every access). On
// AMD that XOR is emitted in the address path; a plain Shared<T> tile addresses
// linearly with no such XOR. Two checks:
//   (1) ISA differential (gfx1151, GPU-free): the swizzled kernel's ISA carries a
//       `v_xor` in the tile-index computation; the byte-identical plain-Shared
//       kernel carries none.
//   (2) on-device (gfx1151): a swizzled tile written then read cross-lane round-
//       trips every value — the XOR involution holds on real hardware, so the
//       swizzle is transparent at the logical-index level.
//
// The device run skips cleanly when no ROCm/HIP device is present; the ISA check
// is GPU-free.
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

// Two byte-identical staging kernels differing only in the tile's declared type:
// `swz` over Swizzled<uint32,16>, `plain` over Shared<uint32>. The cross-lane
// read makes the staging observable; `round` is the on-device involution check.
const char* kSwzSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Swizzled;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void swz(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Swizzled<uint32, 16> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        tile[t] = in[t];\n"
    "        Barrier.workgroup();\n"
    "        out[t] = tile[t];\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void plain(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Shared<uint32> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        tile[t] = in[t];\n"
    "        Barrier.workgroup();\n"
    "        out[t] = tile[t];\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void round(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {\n"
    "        Swizzled<uint32, 16> tile = shared uint32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        tile[t] = in[t];\n"
    "        Barrier.workgroup();\n"
    "        out[t] = tile[(t * 13 + 5) & 255];\n"
    "    }\n"
    "}\n";

// A 16x16 f16 GEMM whose A and B tiles are staged into Swizzled<float16,16> LDS
// tiles (CoopStage.panel), then loaded into WMMA fragments from those swizzled
// tiles. The native AMD coopMatrixLoad must apply the same fragment-coord swizzle
// the staging used, or the matmul is wrong. One wave (32 lanes) on the single tile.
const char* kSwzWmmaSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Swizzled;\n"
    "import cajeta.xpu.CoopStage;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        Swizzled<float16, 16> sa = shared float16[256];\n"
    "        Swizzled<float16, 16> sb = shared float16[256];\n"
    "        CoopStage.panel(sa, a, 0, 0, 16, 16, 16);\n"
    "        CoopStage.panel(sb, b, 0, 0, 16, 16, 16);\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(sa, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(sb, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_swizzle_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_swizzle_arch_" + std::to_string(rng()));
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

std::string emitIsa(const char* kernel) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSwzSrc);
    auto method = findMethod(module->getStructures()["test.M"], kernel);
    if (!method) return {};
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return {};
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_swizzle_isa", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    if (!lowerKernel(method, deviceModule)) return {};
    return cajeta::xpu::amd::emitIsa(deviceModule, *tm);
}

} // namespace

// U3.1.b (ISA differential, GPU-free): the Swizzled kernel emits a `v_xor` in the
// tile-address path that the byte-identical plain-Shared kernel does not — the
// conflict-free permutation is actually applied on AMD.
TEST(XpuSwizzledAmdDeviceTests, swizzleEmitsXorPlainDoesNot) {
    std::string swz = emitIsa("swz");
    std::string plain = emitIsa("plain");
    ASSERT_FALSE(swz.empty()) << "ISA emit failed for the swizzled kernel";
    ASSERT_FALSE(plain.empty()) << "ISA emit failed for the plain kernel";
    EXPECT_NE(swz.find("v_xor"), std::string::npos)
        << "expected the conflict-free XOR in the swizzled tile address path\n" << swz;
    EXPECT_EQ(plain.find("v_xor"), std::string::npos)
        << "a plain Shared tile addresses linearly — no swizzle XOR expected\n" << plain;
}

// U3.1.a (on-device): a Swizzled tile written then read cross-lane round-trips
// every value on gfx1151 — the XOR involution holds on real hardware, so the
// swizzle is transparent (out[t] == in[(t*13+5)&255]).
TEST(XpuSwizzledAmdDeviceTests, swizzledRoundTripsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kSwzSrc);
    auto method = findMethod(module->getStructures()["test.M"], "round");
    ASSERT_NE(method, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_swizzle_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(method, deviceModule), nullptr);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 256;
    std::vector<uint32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = i * 7 + 1;

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "round");
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

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (i * 13 + 5) & 255;
        EXPECT_EQ(out[i], j * 7 + 1) << "lane " << i << " swizzle round-trip wrong";
    }
}

// U3b (ISA, GPU-free): the swizzled-staged WMMA kernel emits both the WMMA
// intrinsic and a `v_xor` (the fragment-coord swizzle) and compiles cleanly on
// gfx1151 — no Cannot-select.
TEST(XpuSwizzledAmdDeviceTests, swizzledWmmaEmitsXorAndWmma) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSwzWmmaSrc);
    auto method = findMethod(module->getStructures()["test.M"], "wmma");
    ASSERT_NE(method, nullptr);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_swz_wmma_isa", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(method, dev), nullptr);
    std::string isa = cajeta::xpu::amd::emitIsa(dev, *tm);
    ASSERT_FALSE(isa.empty()) << "ISA emit failed";
    EXPECT_NE(isa.find("v_wmma"), std::string::npos) << "expected a WMMA op\n" << isa;
    EXPECT_NE(isa.find("v_xor"), std::string::npos)
        << "expected the fragment-coord swizzle XOR\n" << isa;
}

// U3b (on-device): a 16x16 f16 GEMM staged through Swizzled LDS tiles and read
// into WMMA fragments computes correctly on gfx1151 — the native WMMA fragment
// loads apply the same swizzle as the staging, so the result matches the reference.
TEST(XpuSwizzledAmdDeviceTests, swizzledStagedWmmaOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kSwzWmmaSrc);
    auto method = findMethod(module->getStructures()["test.M"], "wmma");
    ASSERT_NE(method, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_swz_wmma_device", ctx);
    configureDeviceModule(dev, *tm);
    ASSERT_NE(lowerKernel(method, dev), nullptr);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    constexpr unsigned NN = 16, TILE = NN * NN;
    std::vector<_Float16> hostA(TILE), hostB(TILE);
    std::vector<float> ref(TILE, 0.0f);
    for (unsigned i = 0; i < NN; ++i)
        for (unsigned k = 0; k < NN; ++k)
            hostA[i * NN + k] = (_Float16) ((i + 2 * k) % 5);
    for (unsigned k = 0; k < NN; ++k)
        for (unsigned j = 0; j < NN; ++j)
            hostB[k * NN + j] = (_Float16) ((3 * k + j) % 4);
    for (unsigned i = 0; i < NN; ++i)
        for (unsigned j = 0; j < NN; ++j) {
            float acc = 0.0f;
            for (unsigned k = 0; k < NN; ++k)
                acc += (float) hostA[i * NN + k] * (float) hostB[k * NN + j];
            ref[i * NN + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "wmma");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(TILE * sizeof(_Float16));
    HipDevicePtr dB = hip.alloc(TILE * sizeof(_Float16));
    HipDevicePtr dC = hip.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, nullptr); ASSERT_NE(dB, nullptr); ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> seed(TILE, -1.0f);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), TILE * sizeof(float)));

    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/32, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> out(TILE, 0.0f);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, TILE * sizeof(float)));
    hip.free(dA); hip.free(dB); hip.free(dC);

    for (unsigned i = 0; i < TILE; ++i)
        EXPECT_EQ(out[i], ref[i]) << "C[" << (i / NN) << "][" << (i % NN) << "] wrong";
}
