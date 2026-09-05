// Tile<T,R,C> — the author-facing cooperative fragment of the cajeta.xpu
// cooperative-tile surface (xpu-cooperative-tile §4, Phase A Unit 3.2.1). A
// `Tile<T,R,C>` is `CooperativeMatrix<T,R,C,Use>` with the SPIR-V "Use" hidden:
// the author declares three type parameters (dtype, rows, cols) and NEVER the
// A/B/accumulator role. The compiler infers each tile's Use from its position in
// the `Group.mac(acc, a, b)` call — acc→Accumulator(2), a→MatrixA(0),
// b→MatrixB(1) — so the same algorithm names no `Use`, no `mma`, no WMMA (§4.1).
//
// The proof is three-fold: (1) a Tile f16 mac still reaches the gfx1151 WMMA
// unit (Use inference picks the tensor core), (2) the Tile kernel's device IR is
// BYTE-IDENTICAL to the equivalent CooperativeMatrix kernel with explicit Use —
// so inference is not an approximation, it reconstructs the exact same slots —
// and (3) it computes correctly end to end on the AMD WMMA cores and on the CPU
// software tile (f32, the portable tier), from one source.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta_test::CajetaJit;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;  // 256

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
              / ("cajeta_xpu_tiletype_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_tiletype_arch_" + std::to_string(rng()));
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

// The device IR of `wmma` in `source`, lowered for gfx1151. Prints the FUNCTION
// (not the module), so module name / temp-path metadata never enters the string
// — what remains is the lowering itself.
std::string amdFunctionIr(const std::string& source) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], "wmma");
    if (!k) return "<no kernel>";
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) return "<no target machine>";
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_tiletype_ir", ctx);
    configureDeviceModule(dev, *tm);
    llvm::Function* f = lowerKernel(k, dev);
    if (!f) return "<lowering failed>";
    std::string ir;
    { llvm::raw_string_ostream os(ir); f->print(os); }
    return ir;
}

// One 16x16x16 f16->f32 tile GEMM (C = A*B), Tile<T,R,C> WITH NO Use spelled and
// `Group.mac`. This is the whole 3.2.1 surface in one kernel.
const char* kTileF16Src =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.Tile;\n"
    "import cajeta.xpu.Group;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        Tile<float16,16,16> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        Tile<float16,16,16> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        Tile<float32,16,16> mc;\n"
    "        mc.splat(0.0f);\n"
    "        Group.mac(mc, ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

// The SAME kernel with the explicit CooperativeMatrix<T,R,C,Use> type and the
// Use spelled out. Everything else — including `Group.mac` — is identical, so a
// byte-for-byte IR match isolates exactly one variable: Use inferred vs Use
// spelled. If they match, inference is faithful, not approximate.
const char* kCoopF16Src =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.Group;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(KernelBuffer<float16> a, KernelBuffer<float16> b,\n"
    "                            KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        Group.mac(mc, ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

// A Tile GEMM in f32 — on every backend with no native f32 matrix config this is
// the PORTABLE software tile (§4.3), so it runs on the CPU backend. run() checks
// the result against the dense reference and returns the mismatch count (0 =
// pass). Integer-valued f32 inputs ⇒ the accumulate is exact. Launch geometry
// from the descriptor: one group per output tile, Group.laneBlock() wide.
const char* kTileF32RunSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Tile;\n"
    "import cajeta.xpu.Group;\n"
    "public class Tf {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> a, KernelBuffer<float32> b,\n"
    "                         KernelBuffer<float32> c) {\n"
    "        Tile<float32,16,16> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        Tile<float32,16,16> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        Tile<float32,16,16> mc;\n"
    "        mc.splat(0.0f);\n"
    "        Group.mac(mc, ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        float32[] ha = heap float32[256];\n"
    "        float32[] hb = heap float32[256];\n"
    "        for (int32 i = 0; i < 16; i = i + 1) {\n"
    "            for (int32 kk = 0; kk < 16; kk = kk + 1) {\n"
    "                ha[i * 16 + kk] = (float32) ((i + 2 * kk) % 5 - 2);\n"
    "                hb[i * 16 + kk] = (float32) ((3 * i + kk) % 4 - 1);\n"
    "            }\n"
    "        }\n"
    "        KernelBuffer<float32> da = heap KernelBuffer<float32>(0, 256);\n"
    "        KernelBuffer<float32> db = heap KernelBuffer<float32>(0, 256);\n"
    "        KernelBuffer<float32> dc = heap KernelBuffer<float32>(0, 256);\n"
    "        da.allocate();\n"
    "        db.allocate();\n"
    "        dc.allocate();\n"
    "        da.upload(ha);\n"
    "        db.upload(hb);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [Group.laneBlock()])(da, db, dc);\n"
    "        s.sync();\n"
    "        float32[] hc = heap float32[256];\n"
    "        dc.download(hc);\n"
    "        da.free();\n"
    "        db.free();\n"
    "        dc.free();\n"
    "        int32 bad = 0;\n"
    "        for (int32 i = 0; i < 16; i = i + 1) {\n"
    "            for (int32 j = 0; j < 16; j = j + 1) {\n"
    "                float32 ref = 0.0f;\n"
    "                for (int32 kk = 0; kk < 16; kk = kk + 1) {\n"
    "                    ref = ref + ha[i * 16 + kk] * hb[kk * 16 + j];\n"
    "                }\n"
    "                float32 d = hc[i * 16 + j] - ref;\n"
    "                if (d < 0.0f) { d = -d; }\n"
    "                if (d > 0.001f) { bad = bad + 1; }\n"
    "            }\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

// 3.2.1 (emit): a Tile f16 mac reaches the gfx1151 WMMA intrinsic — the author
// declared Tile<float16,16,16> (three params, NO Use) and named `mac`, and the
// compiler still chose the tensor core.
TEST(XpuTileTypeDevice, tileHidesUseEmitsAmdWmma) {
    std::string ir = amdFunctionIr(kTileF16Src);
    EXPECT_NE(ir.find("wmma.f32.16x16x16.f16"), std::string::npos)
        << "Tile<float16,16,16> mac must reach the gfx1151 WMMA unit (Use "
           "inferred, never spelled)\n" << ir;
}

// 3.2.1 (faithfulness): the Tile kernel's device IR is byte-identical to the
// CooperativeMatrix-with-explicit-Use kernel's. Inference reconstructs the exact
// same A(0)/B(1)/accumulator(2) slots — it is not an approximation, and the
// author pays nothing for hiding Use.
TEST(XpuTileTypeDevice, tileLoweringIdenticalToCooperativeMatrix) {
    std::string tileIr = amdFunctionIr(kTileF16Src);
    std::string coopIr = amdFunctionIr(kCoopF16Src);
    ASSERT_NE(tileIr.find("wmma"), std::string::npos) << tileIr;
    EXPECT_EQ(tileIr, coopIr)
        << "Tile<T,R,C> lowering diverged from CooperativeMatrix<T,R,C,Use> — "
           "Use inference is not faithful.\n--- Tile ---\n" << tileIr
        << "\n--- CooperativeMatrix ---\n" << coopIr;
}

// 3.2.1 (AMD device): the Tile f16 GEMM computes correctly on the WMMA cores.
// Non-uniform, exact-integer inputs make the result i,j-distinct and
// layout-sensitive, so a mis-inferred Use (A/B swapped, accumulator mistaken)
// would not match the host reference.
TEST(XpuTileTypeDevice, tileF16MatmulOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kTileF16Src);
    auto k = findMethod(module->getStructures()["test.M"], "wmma");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_tiletype_f16_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    // Native _Float16 host storage (the exact conversion the WMMA cores see);
    // non-negative, i,j-distinct, exact-integer data (products <= 12, 16-term
    // sum <= 192, all exact in f16/f32), so the layout check is bit-exact.
    std::vector<_Float16> hostA(TILE), hostB(TILE);
    std::vector<float> ref(TILE, 0.0f);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            hostB[kk * N + j] = (_Float16) ((3 * kk + j) % 4);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += (float) hostA[i * N + kk] * (float) hostB[kk * N + j];
            ref[i * N + j] = acc;
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
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> seed(TILE, -1.0f);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), TILE * sizeof(float)));

    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/32, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> out(TILE, -2.0f);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, TILE * sizeof(float)));
    hip.free(dA);
    hip.free(dB);
    hip.free(dC);

    size_t mismatches = 0;
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            if (out[i * N + j] != ref[i * N + j] && mismatches++ < 8)
                ADD_FAILURE() << "Tile f16 WMMA mismatch at (" << i << "," << j
                              << "): got " << out[i * N + j] << " want "
                              << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u)
        << "Tile f16 WMMA GEMM had " << mismatches << " mismatches";
}

// 3.2.1 (CPU): the SAME Tile surface on the portable software tile (f32 has no
// native matrix config, so this is the §4.3 fallback), one work-item per tile.
// Correctness here is the guard that no WMMA/wave assumption leaked into the
// Tile type — the author's Use-free source is portable.
TEST(XpuTileTypeDevice, tileF32MatmulOnCpu) {
    auto jit = CajetaJit::compile(kTileF32RunSrc, "test.Tf", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "Tile<float32,16,16> GEMM disagreed with the dense reference on the "
           "CPU software tile";
}

// 3.2.1 (AMD device, portable tier): the f32 Tile GEMM also runs on AMD, where
// it takes the software tile (the f32 accumulator is native but the f32 A/B
// operands are not, so the GEMM straddles and demotes as a group — §4.4).
TEST(XpuTileTypeDevice, tileF32MatmulOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kTileF32RunSrc, "test.Tf", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "Tile<float32,16,16> GEMM disagreed with the dense reference on AMD "
           "(portable tier)";
}
