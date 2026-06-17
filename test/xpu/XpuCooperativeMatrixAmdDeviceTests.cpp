//
// CooperativeMatrix on AMD — RDNA3 WMMA matrix cores (cajeta-gpu CM7), end to end
// on a real gfx1151 (Strix Halo). The AMD-native counterpart of the Vulkan
// cooperative-matrix device tests: the SAME @Kernel that loads an A tile (use 0)
// and a B tile (use 1), zeroes an accumulator C (use 2), issues one `mma`, and
// stores C — but here it lowers through AmdgpuKernelLowering to the hand-marshalled
// `v_wmma_f32_16x16x16_f16` path (wave32) and DISPATCHES on the WMMA cores.
//
// This is the LAYOUT verification: the inputs vary with both indices (A[i][k] =
// (i+2k)%5, B[k][j] = (3k+j)%4), so the result C[i][j] is i,j-distinct. A wrong
// fragment layout — A or B loaded transposed, or the wave32 accumulator rows
// mis-interleaved on store — would NOT match the CPU reference. Small integers
// exact in half (products <= 12, 16-term sum <= 192), so the check is exact.
//
// Skips cleanly when no ROCm/HIP device is present.
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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;  // 256

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_wmmadev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_wmmadev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
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

// Host bfloat16 <-> float (bf16 = the high 16 bits of an IEEE float).
uint16_t f2bf16(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    uint32_t r = b + 0x7FFFu + ((b >> 16) & 1u);
    return (uint16_t) (r >> 16);
}
float bf162f(uint16_t h) {
    uint32_t b = (uint32_t) h << 16;
    float f;
    __builtin_memcpy(&f, &b, 4);
    return f;
}

const char* kF16Source =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(GpuBuffer<float16> a, GpuBuffer<float16> b,\n"
    "                            GpuBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuCooperativeMatrixAmdDeviceTests, f16WmmaMatmulOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kF16Source);
    auto k = findMethod(module->getStructures()["test.M"], "wmma");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wmma_f16_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

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

    // One wave (32 lanes) cooperates on the single 16x16 tile.
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
                ADD_FAILURE() << "WMMA mismatch at (" << i << "," << j << "): got "
                              << out[i * N + j] << " want " << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u) << "f16 WMMA tile had " << mismatches << " mismatches";
}

// bfloat16 A/B -> f32 accumulate, the ML training config. The headline of CM7:
// bf16 has no Vulkan cooperative-matrix config (CM6 runs it in software there),
// but RDNA3.5 WMMA does bf16 natively — `v_wmma_f32_16x16x16_bf16`. SAME layout
// as the f16 path (A row, B col, interleaved-row accumulator), different
// intrinsic + bf16-as-i16 fragments. Same non-uniform, exact-integer check.
const char* kBf16Source =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmmabf(GpuBuffer<bfloat16> a, GpuBuffer<bfloat16> b,\n"
    "                              GpuBuffer<float32> c) {\n"
    "        CooperativeMatrix<bfloat16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<bfloat16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixAmdDeviceTests, bf16WmmaMatmulOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kBf16Source);
    auto k = findMethod(module->getStructures()["test.M"], "wmmabf");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wmma_bf16_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<uint16_t> hostA(TILE), hostB(TILE);
    std::vector<float> ref(TILE, 0.0f);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = f2bf16((float) ((i + 2 * kk) % 5));
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            hostB[kk * N + j] = f2bf16((float) ((3 * kk + j) % 4));
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += bf162f(hostA[i * N + kk]) * bf162f(hostB[kk * N + j]);
            ref[i * N + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "wmmabf");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(TILE * sizeof(uint16_t));
    HipDevicePtr dB = hip.alloc(TILE * sizeof(uint16_t));
    HipDevicePtr dC = hip.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), TILE * sizeof(uint16_t)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), TILE * sizeof(uint16_t)));
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
                ADD_FAILURE() << "bf16 WMMA mismatch at (" << i << "," << j
                              << "): got " << out[i * N + j] << " want "
                              << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u) << "bf16 WMMA tile had " << mismatches
                              << " mismatches";
}

// int8 A/B -> int32 accumulate, the quantized-inference config (CM8). RDNA3.5
// WMMA does this natively — `v_wmma_i32_16x16x16_iu8`: the 16 K-values pack
// 4-per-i32 into a <4 x i32> A/B fragment, the accumulator is <8 x i32>, and the
// intrinsic carries signedness + clamp operands. We use SIGNED, non-uniform data
// spanning negatives (A in -2..2, B in -1..2) so the test exercises the signed
// path AND is layout-sensitive: a transposed fragment or mis-packed i32 word
// would not match the int32 host reference. Exact integer arithmetic throughout.
const char* kInt8Source =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmmai8(GpuBuffer<int8> a, GpuBuffer<int8> b,\n"
    "                              GpuBuffer<int32> c) {\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.splat(0);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixAmdDeviceTests, int8WmmaMatmulOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kInt8Source);
    auto k = findMethod(module->getStructures()["test.M"], "wmmai8");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_wmma_int8_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<int8_t> hostA(TILE), hostB(TILE);
    std::vector<int32_t> ref(TILE, 0);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (int8_t) ((int) ((i + 2 * kk) % 5) - 2);
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            hostB[kk * N + j] = (int8_t) ((int) ((3 * kk + j) % 4) - 1);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            int32_t acc = 0;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += (int32_t) hostA[i * N + kk] * (int32_t) hostB[kk * N + j];
            ref[i * N + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "wmmai8");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(TILE * sizeof(int8_t));
    HipDevicePtr dB = hip.alloc(TILE * sizeof(int8_t));
    HipDevicePtr dC = hip.alloc(TILE * sizeof(int32_t));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), TILE * sizeof(int8_t)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), TILE * sizeof(int8_t)));
    std::vector<int32_t> seed(TILE, -1);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), TILE * sizeof(int32_t)));

    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/32, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<int32_t> out(TILE, -2);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, TILE * sizeof(int32_t)));
    hip.free(dA);
    hip.free(dB);
    hip.free(dC);

    size_t mismatches = 0;
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            if (out[i * N + j] != ref[i * N + j] && mismatches++ < 8)
                ADD_FAILURE() << "int8 WMMA mismatch at (" << i << "," << j
                              << "): got " << out[i * N + j] << " want "
                              << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u) << "int8 WMMA tile had " << mismatches
                              << " mismatches";
}

// Multi-tile GEMM: a real C[16x16] = A[16x32] · B[32x16] — TWO K-tiles summed
// into ONE accumulator across a K-loop. This is the step from "one mma" to "a
// GEMM": it exercises the two axes the single-tile tests don't —
//   (1) offset-addressed sub-tiles: A's K-tile kt starts at element kt*16 with
//       stride 32 (the full A row width, NOT the 16-wide tile), so the load
//       gathers a 16x16 window out of a wider row-major matrix; B's K-tile kt
//       starts at row kt*16 (element kt*256), stride 16;
//   (2) K-accumulation: the accumulator is zeroed once, then each loop iteration
//       issues mma(ta, tb) reading+writing it in place, so the two tile products
//       sum — the inner-product over K=32 split across tiles.
// Non-uniform data keeps it i,j-distinct and layout-sensitive; K=32 sum of small
// products stays exact in f32.
const char* kGemmKLoopSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemmk(GpuBuffer<float16> a, GpuBuffer<float16> b,\n"
    "                             GpuBuffer<float32> c) {\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        acc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> ta;\n"
    "        CooperativeMatrix<float16,16,16,1> tb;\n"
    "        for (int32 kt = 0; kt < 2; kt = kt + 1) {\n"
    "            ta.load(a, kt * 16, 0, 32);\n"
    "            tb.load(b, kt * 256, 0, 16);\n"
    "            acc.mma(ta, tb);\n"
    "        }\n"
    "        acc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixAmdDeviceTests, gemmKLoopOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    constexpr unsigned M = 16, Kdim = 32, Ndim = 16;
    constexpr unsigned ASZ = M * Kdim, BSZ = Kdim * Ndim, CSZ = M * Ndim;

    Compiler compiler;
    auto module = compileForInspection(compiler, kGemmKLoopSource);
    auto k = findMethod(module->getStructures()["test.M"], "gemmk");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_gemm_kloop_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<_Float16> hostA(ASZ), hostB(BSZ);
    std::vector<float> ref(CSZ, 0.0f);
    for (unsigned i = 0; i < M; ++i)
        for (unsigned kk = 0; kk < Kdim; ++kk)
            hostA[i * Kdim + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < Kdim; ++kk)
        for (unsigned j = 0; j < Ndim; ++j)
            hostB[kk * Ndim + j] = (_Float16) ((3 * kk + j) % 4);
    for (unsigned i = 0; i < M; ++i)
        for (unsigned j = 0; j < Ndim; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < Kdim; ++kk)
                acc += (float) hostA[i * Kdim + kk] * (float) hostB[kk * Ndim + j];
            ref[i * Ndim + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "gemmk");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(ASZ * sizeof(_Float16));
    HipDevicePtr dB = hip.alloc(BSZ * sizeof(_Float16));
    HipDevicePtr dC = hip.alloc(CSZ * sizeof(float));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), ASZ * sizeof(_Float16)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), BSZ * sizeof(_Float16)));
    std::vector<float> seed(CSZ, -1.0f);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), CSZ * sizeof(float)));

    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/32, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> out(CSZ, -2.0f);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, CSZ * sizeof(float)));
    hip.free(dA);
    hip.free(dB);
    hip.free(dC);

    size_t mismatches = 0;
    for (unsigned i = 0; i < M; ++i)
        for (unsigned j = 0; j < Ndim; ++j)
            if (out[i * Ndim + j] != ref[i * Ndim + j] && mismatches++ < 8)
                ADD_FAILURE() << "K-loop GEMM mismatch at (" << i << "," << j
                              << "): got " << out[i * Ndim + j] << " want "
                              << ref[i * Ndim + j];
    EXPECT_EQ(mismatches, 0u) << "K-loop GEMM had " << mismatches << " mismatches";
}

// Full tiled GEMM: C[32x32] = A[32x32] · B[32x32], K=32. The output is a 2x2 grid
// of 16x16 tiles; each tile is computed by its OWN wave (one block = 32 lanes =
// one wave), and within a wave a 2-deep K-loop sums the two K-tiles. This is the
// M/N axis of tiling — the step past the single-output-tile K-loop GEMM:
//   - the launch is grid=4 blocks of 32, so the four waves run concurrently;
//   - each wave reads Workgroup.x() (its block index 0..3) and decodes its output
//     tile (ti = block/2, tj = block%2) — all 32 lanes of a wave agree on it;
//   - the offsets pick that wave's sub-tiles out of the 32-wide row-major matrices
//     (A row-tile ti, B col-tile tj, the shared K walked by the loop).
// Layout-sensitive non-uniform data; K=32 small-integer sums stay exact in f32.
const char* kGemmTiledSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "import cajeta.gpu.Workgroup;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemm(GpuBuffer<float16> a, GpuBuffer<float16> b,\n"
    "                            GpuBuffer<float32> c) {\n"
    "        uint32 tile = Workgroup.x();\n"
    "        uint32 ti = tile / 2;\n"
    "        uint32 tj = tile % 2;\n"
    "        uint32 aBase = ti * 512;\n"        // (ti*16) rows * 32 stride
    "        uint32 bBase = tj * 16;\n"          // tj-th 16-col band
    "        uint32 cBase = ti * 512 + tj * 16;\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        acc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> ta;\n"
    "        CooperativeMatrix<float16,16,16,1> tb;\n"
    "        for (uint32 kt = 0; kt < 2; kt = kt + 1) {\n"
    "            ta.load(a, aBase + kt * 16, 0, 32);\n"   // A sub-tile (ti, kt)
    "            tb.load(b, bBase + kt * 512, 0, 32);\n"   // B sub-tile (kt, tj)
    "            acc.mma(ta, tb);\n"
    "        }\n"
    "        acc.store(c, cBase, 0, 32);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixAmdDeviceTests, gemmOutputTilesOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    constexpr unsigned D = 32;             // M = N = K = 32 (a 2x2x2 tile grid)
    constexpr unsigned SZ = D * D;         // 1024
    constexpr unsigned NTILES = (D / 16) * (D / 16);  // 4 output tiles

    Compiler compiler;
    auto module = compileForInspection(compiler, kGemmTiledSource);
    auto k = findMethod(module->getStructures()["test.M"], "gemm");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_gemm_tiled_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<_Float16> hostA(SZ), hostB(SZ);
    std::vector<float> ref(SZ, 0.0f);
    for (unsigned i = 0; i < D; ++i)
        for (unsigned kk = 0; kk < D; ++kk)
            hostA[i * D + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < D; ++kk)
        for (unsigned j = 0; j < D; ++j)
            hostB[kk * D + j] = (_Float16) ((3 * kk + j) % 4);
    for (unsigned i = 0; i < D; ++i)
        for (unsigned j = 0; j < D; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < D; ++kk)
                acc += (float) hostA[i * D + kk] * (float) hostB[kk * D + j];
            ref[i * D + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "gemm");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(SZ * sizeof(_Float16));
    HipDevicePtr dB = hip.alloc(SZ * sizeof(_Float16));
    HipDevicePtr dC = hip.alloc(SZ * sizeof(float));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), SZ * sizeof(_Float16)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), SZ * sizeof(_Float16)));
    std::vector<float> seed(SZ, -1.0f);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), SZ * sizeof(float)));

    // One block (one wave of 32) per output tile; the four run concurrently.
    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/NTILES, /*block=*/32, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> out(SZ, -2.0f);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, SZ * sizeof(float)));
    hip.free(dA);
    hip.free(dB);
    hip.free(dC);

    size_t mismatches = 0;
    for (unsigned i = 0; i < D; ++i)
        for (unsigned j = 0; j < D; ++j)
            if (out[i * D + j] != ref[i * D + j] && mismatches++ < 8)
                ADD_FAILURE() << "tiled GEMM mismatch at (" << i << "," << j
                              << "): got " << out[i * D + j] << " want "
                              << ref[i * D + j];
    EXPECT_EQ(mismatches, 0u) << "tiled GEMM had " << mismatches << " mismatches";
}

// --- LDS staging (Option A): load a coop-matrix tile from a Shared<T> (LDS) tile.
// One wave stages A and B tiles global->LDS with a hand-written cooperative copy
// (NOT CoopStage — this isolates the Shared-source load), barriers, then runs the
// single-tile matmul out of LDS. If load(Shared<T>) addressed the wrong storage
// class the result would be garbage. Non-uniform, exact-integer check.
const char* kSharedLoadSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "import cajeta.gpu.GpuThread;\n"
    "import cajeta.gpu.Workgroup;\n"
    "import cajeta.gpu.Barrier;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void shld(GpuBuffer<float16> a, GpuBuffer<float16> b,\n"
    "                            GpuBuffer<float32> c) {\n"
    "        Shared<float16> sa = shared float16[16 * 16];\n"
    "        Shared<float16> sb = shared float16[16 * 16];\n"
    "        uint32 tid = GpuThread.x();\n"
    "        uint32 nthr = Workgroup.dimX();\n"
    "        for (uint32 e = tid; e < 256; e = e + nthr) {\n"
    "            sa[e] = a[e];\n"
    "            sb[e] = b[e];\n"
    "        }\n"
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

TEST(XpuCooperativeMatrixAmdDeviceTests, sharedLoadMatmulOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kSharedLoadSource);
    auto k = findMethod(module->getStructures()["test.M"], "shld");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_sharedload_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

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
    HipFunction fn = hip.getFunction(mod, "shld");
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
                ADD_FAILURE() << "Shared-load matmul mismatch at (" << i << ","
                              << j << "): got " << out[i * N + j] << " want "
                              << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u) << "Shared-load matmul had " << mismatches
                              << " mismatches";
}

// --- Full LDS-staged GEMM (Options A + B together): C[32x32] = A[32x32].B[32x32].
// One workgroup of 4 waves (block=128) computes the 2x2 output-tile grid. Each
// K-step: CoopStage.panel stages the A row-panel (32x16) and B col-panel (16x32)
// global->LDS ONCE, barrier, then every wave loads ITS operand tiles out of the
// shared panels and accumulates — so each staged element is read by multiple
// waves (the reuse that LDS staging buys). This is the headline of Option B.
const char* kLdsStagedGemmSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.xpu.CooperativeMatrix;\n"
    "import cajeta.gpu.xpu.CoopStage;\n"
    "import cajeta.gpu.GpuThread;\n"
    "import cajeta.gpu.Barrier;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemm(GpuBuffer<float16> a, GpuBuffer<float16> b,\n"
    "                            GpuBuffer<float32> c) {\n"
    "        uint32 wave = GpuThread.x() / 32;\n"
    "        uint32 ti = wave / 2;\n"
    "        uint32 tj = wave % 2;\n"
    "        Shared<float16> sa = shared float16[32 * 16];\n"
    "        Shared<float16> sb = shared float16[16 * 32];\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        acc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> wa;\n"
    "        CooperativeMatrix<float16,16,16,1> wb;\n"
    "        for (uint32 kt = 0; kt < 2; kt = kt + 1) {\n"
    "            CoopStage.panel(sa, a, 0, kt * 16, 32, 16, 32);\n"
    "            CoopStage.panel(sb, b, kt * 16, 0, 16, 32, 32);\n"
    "            Barrier.workgroup();\n"
    "            wa.load(sa, ti * 256, 0, 16);\n"
    "            wb.load(sb, tj * 16, 0, 32);\n"
    "            acc.mma(wa, wb);\n"
    "            Barrier.workgroup();\n"
    "        }\n"
    "        acc.store(c, ti * 512 + tj * 16, 0, 32);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixAmdDeviceTests, ldsStagedGemmOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    constexpr unsigned D = 32;
    constexpr unsigned SZ = D * D;

    Compiler compiler;
    auto module = compileForInspection(compiler, kLdsStagedGemmSource);
    auto k = findMethod(module->getStructures()["test.M"], "gemm");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_lds_staged_gemm_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<_Float16> hostA(SZ), hostB(SZ);
    std::vector<float> ref(SZ, 0.0f);
    for (unsigned i = 0; i < D; ++i)
        for (unsigned kk = 0; kk < D; ++kk)
            hostA[i * D + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < D; ++kk)
        for (unsigned j = 0; j < D; ++j)
            hostB[kk * D + j] = (_Float16) ((3 * kk + j) % 4);
    for (unsigned i = 0; i < D; ++i)
        for (unsigned j = 0; j < D; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < D; ++kk)
                acc += (float) hostA[i * D + kk] * (float) hostB[kk * D + j];
            ref[i * D + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "gemm");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(SZ * sizeof(_Float16));
    HipDevicePtr dB = hip.alloc(SZ * sizeof(_Float16));
    HipDevicePtr dC = hip.alloc(SZ * sizeof(float));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), SZ * sizeof(_Float16)));
    ASSERT_TRUE(hip.memcpyHtoD(dB, hostB.data(), SZ * sizeof(_Float16)));
    std::vector<float> seed(SZ, -1.0f);
    ASSERT_TRUE(hip.memcpyHtoD(dC, seed.data(), SZ * sizeof(float)));

    // One workgroup of 4 waves (128 threads); the waves share the staged panels.
    void* params[] = { &dA, &dB, &dC };
    ASSERT_TRUE(hip.launch(fn, /*grid=*/1, /*block=*/128, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> out(SZ, -2.0f);
    ASSERT_TRUE(hip.memcpyDtoH(out.data(), dC, SZ * sizeof(float)));
    hip.free(dA);
    hip.free(dB);
    hip.free(dC);

    size_t mismatches = 0;
    for (unsigned i = 0; i < D; ++i)
        for (unsigned j = 0; j < D; ++j)
            if (out[i * D + j] != ref[i * D + j] && mismatches++ < 8)
                ADD_FAILURE() << "LDS-staged GEMM mismatch at (" << i << "," << j
                              << "): got " << out[i * D + j] << " want "
                              << ref[i * D + j];
    EXPECT_EQ(mismatches, 0u) << "LDS-staged GEMM had " << mismatches
                              << " mismatches";
}
