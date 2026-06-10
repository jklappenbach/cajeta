//
// CooperativeMatrix<T,Rows,Cols,Use> (cajeta-gpu CM5) — the SPV_KHR_cooperative_
// matrix tile-matmul primitive, end to end on a REAL device. The on-device
// counterpart of the CM4/CM5b emit tests: the SAME cajeta @Kernel that loads an
// A tile (use 0) and a B tile (use 1), zeroes an accumulator C (use 2), issues
// one OpCooperativeMatrixMulAddKHR, and stores C — compiles through the Vulkan/
// SPIR-V backend and DISPATCHES on the RADV STRIX_HALO WMMA cores.
//
// MIXED PRECISION (the only float config the hardware exposes, and the SPELA/
// Toffee regime): A and B are float16 (IEEE half), the accumulator/result is
// float32. The device lowerer keys each tile off its declared element type, so
// f16-in / f32-accumulate flows through unchanged (CM5a made cajeta's float16 an
// IEEE half so it matches VK_COMPONENT_TYPE_FLOAT16_KHR).
//
// EXACT check: the inputs are small integers that are exactly representable in
// half, so every product and the 16-term f32 accumulation are exact — the device
// tile must equal the CPU reference to the bit. (No fp tolerance; like the wave
// test's exact out[i]==50.)
//
// Gated on VulkanDriver::coopMatrixAvailable() — which checks the device actually
// lists the 16x16x16 Subgroup f16/f16->f32 config — so it SKIPs cleanly on a box
// without WMMA (or without the cooperative-matrix extension at all).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/xpu/lowering/LoweringTarget.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// One 16x16 tile per operand: A (16x16 half), B (16x16 half), C (16x16 float).
constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;  // 256 elements

// A single-tile matmul C = A * B through the cooperative-matrix verbs. No
// per-thread indexing: load/mma/store are subgroup-collective, so the whole
// subgroup cooperates on one tile (the dispatch's 64 threads = full subgroups).
const char* kMatmulSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void matmul(Buffer<float16> a, Buffer<float16> b,\n"
    "                              Buffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"           // layout 0 = row-major, stride 16
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"              // zero accumulator
    "        mc.mma(ma, mb);\n"             // C = A*B + C  (one tile FMA)
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_coopmatdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_coopmatdev_arch_" + std::to_string(rng()));
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

// Host bfloat16 <-> float (bf16 = the high 16 bits of an IEEE float). Small
// integers (|x| < 256) are exact in bf16, so the GEMM check below is exact.
uint16_t f2bf16(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    uint32_t rounded = b + 0x7FFFu + ((b >> 16) & 1u);  // round to nearest even
    return (uint16_t) (rounded >> 16);
}
float bf162f(uint16_t h) {
    uint32_t b = (uint32_t) h << 16;
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

// RAII setenv guard — set CAJETA_GPU_<FEATURE>_IMPL for the scope, restore on
// destruction (env is process-global; gtest runs all tests in one process, so a
// leak would perturb sibling tests). Survives a gtest ASSERT early-return.
struct EnvGuard {
    std::string name;
    EnvGuard(const char* var, const char* value) : name(var) {
        setenv(var, value, /*overwrite=*/1);
    }
    ~EnvGuard() { unsetenv(name.c_str()); }
};

} // namespace

TEST(XpuCooperativeMatrixDeviceTests, mixedPrecisionMatmulOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config";
    }

    Compiler compiler;
    auto module = compileForInspection(compiler, kMatmulSource);
    auto k = findMethod(module->getStructures()["test.M"], "matmul");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // Row-major tiles. Inputs are small integers exactly representable in half
    // (A in 0..4, B in 0..3): products <= 12, the 16-term sum <= 192 — all exact
    // in both half and float, so the device tile equals the integer reference.
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

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(TILE * sizeof(_Float16));
    auto dB = vk.alloc(TILE * sizeof(_Float16));
    auto dC = vk.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> zero(TILE, -1.0f);
    ASSERT_TRUE(vk.upload(dC, zero.data(), TILE * sizeof(float)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "matmul", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<float> out(TILE, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, TILE * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "tile mismatch at (" << i << "," << j << ")";
}

// GEMM-1 — K-accumulation across multiple tiles via OFFSET addressing. One 16x16
// output tile, but K=32 so it sums two 16-wide K-tiles in a for-loop, each loaded
// from a runtime offset into a larger row-major matrix: C[16x16] = A[16x32] *
// B[32x16]. Proves the new `load(buf, offset, layout, stride)` offset arg + the
// cooperative-matrix accumulator persisting across loop iterations (mma is
// C = A*B + C in place) — the inner-K core of a real tiled GEMM. Still one
// workgroup (M/N tiling is GEMM-2). Exact integer check.
const char* kKAccumSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemm2k(Buffer<float16> a, Buffer<float16> b,\n"
    "                              Buffer<float32> c) {\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        for (uint32 kk = 0; kk < 2; kk += 1) {\n"
    "            ma.load(a, kk * 16, 0, 32);\n"    // A tile (0,kk): row-major stride K=32
    "            mb.load(b, kk * 256, 0, 16);\n"   // B tile (kk,0): row-major stride N=16
    "            mc.mma(ma, mb);\n"               // C += A_k * B_k
    "        }\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixDeviceTests, kAccumulationMatmulOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config";
    }

    Compiler compiler;
    auto module = compileForInspection(compiler, kKAccumSource);
    auto k = findMethod(module->getStructures()["test.M"], "gemm2k");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_kacc_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // A is 16x32, B is 32x16, C is 16x16. Small integers exact in half (A,B in
    // 0..2): per-term <= 4, the 32-term sum <= 128 — exact in half and float.
    constexpr unsigned K = 32;
    std::vector<_Float16> hostA(N * K), hostB(K * N);
    std::vector<float> ref(N * N, 0.0f);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < K; ++kk)
            hostA[i * K + kk] = (_Float16) ((i + 2 * kk) % 3);
    for (unsigned kk = 0; kk < K; ++kk)
        for (unsigned j = 0; j < N; ++j)
            hostB[kk * N + j] = (_Float16) ((3 * kk + j) % 3);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < K; ++kk)
                acc += (float) hostA[i * K + kk] * (float) hostB[kk * N + j];
            ref[i * N + j] = acc;
        }

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(N * K * sizeof(_Float16));
    auto dB = vk.alloc(K * N * sizeof(_Float16));
    auto dC = vk.alloc(N * N * sizeof(float));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), N * K * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), K * N * sizeof(_Float16)));
    std::vector<float> zero(N * N, -1.0f);
    ASSERT_TRUE(vk.upload(dC, zero.data(), N * N * sizeof(float)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "gemm2k", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<float> out(N * N, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, N * N * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "K-accum mismatch at (" << i << "," << j << ")";
}

// GEMM-2 — a full tiled M×N×K GEMM: C[rows×cols] = A[rows×depth] * B[depth×cols].
// Each workgroup owns one 16×16 output tile (decoded from Workgroup.x()), zeroes
// an accumulator, and loops over the depth dimension issuing one mma per K-tile —
// the canonical cooperative-matrix matmul. rows/cols/depth arrive as scalar args
// (lowered to single-element storage buffers on Vulkan), so the kernel is generic
// over size, not baked. Dispatched as (rows/16)*(cols/16) workgroups. This is the
// matmul SPELA/Toffee can call. Exact integer check over a 64×64×64 problem.
const char* kGemmSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.CooperativeMatrix;\n"
    "import cajeta.gpu.core.Workgroup;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void gemm(Buffer<float16> a, Buffer<float16> b,\n"
    "                            Buffer<float32> c,\n"
    "                            uint32 rows, uint32 cols, uint32 depth) {\n"
    // Tile offsets are written naturally inline (mi*16*depth etc.). The shared
    // loop-invariant subexpressions (mi*16, nj*16) used in both the loop body and
    // the post-loop store are exactly what MachineCSE commons into the loop header
    // — which used to land *after* the OpLoopMerge and produce invalid structured
    // CFG (spirv-val reject + RADV hang). The fork's SPIRVFixupMergePlacement pass
    // now re-seats the merge, so the kernel needs no manual offset hoisting.
    "        uint32 ntiles = cols / 16;\n"
    "        uint32 wid = Workgroup.x();\n"
    "        uint32 mi = wid / ntiles;\n"        // output row-tile
    "        uint32 nj = wid % ntiles;\n"        // output col-tile
    "        uint32 ktiles = depth / 16;\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        uint32 kk = 0;\n"
    "        for (kk = 0; kk < ktiles; kk += 1) {\n"
    "            ma.load(a, mi * 16 * depth + kk * 16, 0, depth);\n"
    "            mb.load(b, kk * 16 * cols + nj * 16, 0, cols);\n"
    "            mc.mma(ma, mb);\n"
    "        }\n"
    "        mc.store(c, mi * 16 * cols + nj * 16, 0, cols);\n"
    "    }\n"
    "}\n";

TEST(XpuCooperativeMatrixDeviceTests, tiledGemmOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config";
    }

    Compiler compiler;
    auto module = compileForInspection(compiler, kGemmSource);
    auto k = findMethod(module->getStructures()["test.M"], "gemm");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_gemm_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    // 64x64x64. Small integers exact in half (A in 0..1, B in 0..2): per-term
    // <= 2, the 64-term sum <= 128 — exact in half and float.
    constexpr uint32_t M = 64, NN = 64, K = 64;
    std::vector<_Float16> hostA(M * K), hostB(K * NN);
    std::vector<float> ref(M * NN, 0.0f);
    for (uint32_t i = 0; i < M; ++i)
        for (uint32_t kk = 0; kk < K; ++kk)
            hostA[i * K + kk] = (_Float16) ((i * 3 + kk) % 2);
    for (uint32_t kk = 0; kk < K; ++kk)
        for (uint32_t j = 0; j < NN; ++j)
            hostB[kk * NN + j] = (_Float16) ((2 * kk + j) % 3);
    for (uint32_t i = 0; i < M; ++i)
        for (uint32_t j = 0; j < NN; ++j) {
            float acc = 0.0f;
            for (uint32_t kk = 0; kk < K; ++kk)
                acc += (float) hostA[i * K + kk] * (float) hostB[kk * NN + j];
            ref[i * NN + j] = acc;
        }

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(M * K * sizeof(_Float16));
    auto dB = vk.alloc(K * NN * sizeof(_Float16));
    auto dC = vk.alloc(M * NN * sizeof(float));
    auto dRows = vk.alloc(sizeof(uint32_t));
    auto dCols = vk.alloc(sizeof(uint32_t));
    auto dDepth = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_NE(dRows, 0u);
    ASSERT_NE(dCols, 0u);
    ASSERT_NE(dDepth, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), M * K * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), K * NN * sizeof(_Float16)));
    std::vector<float> zero(M * NN, -1.0f);
    ASSERT_TRUE(vk.upload(dC, zero.data(), M * NN * sizeof(float)));
    uint32_t rows = M, cols = NN, depth = K;
    ASSERT_TRUE(vk.upload(dRows, &rows, sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dCols, &cols, sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dDepth, &depth, sizeof(uint32_t)));

    const unsigned grid = (M / 16) * (NN / 16);  // one workgroup per output tile
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "gemm",
                          {dA, dB, dC, dRows, dCols, dDepth}, grid));

    std::vector<float> out(M * NN, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, M * NN * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);
    vk.free(dRows);
    vk.free(dCols);
    vk.free(dDepth);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < M; ++i)
        for (uint32_t j = 0; j < NN; ++j)
            if (out[i * NN + j] != ref[i * NN + j] && mismatches++ < 8)
                ADD_FAILURE() << "GEMM mismatch at (" << i << "," << j << "): got "
                              << out[i * NN + j] << " want " << ref[i * NN + j];
    EXPECT_EQ(mismatches, 0u) << "tiled GEMM had " << mismatches << " mismatches";
}

// CM6 — the SOFTWARE cooperative-matrix fallback. bfloat16 has no Vulkan
// cooperative-matrix config (no driver advertises one — it needs Intel-only
// SPV_INTEL_bfloat16_arithmetic), so a bf16 tile takes the portable flat
// tile-matmul path: a `[16*16 x bf16]` tile, a strided gather, and an honest
// triple-loop `C = A·B` accumulated in f32. This exercises that path end to end
// on the REAL Vulkan device (RADV STRIX_HALO) — proving bf16 GEMM RUNS on a
// driver that exposes no bf16 matrix-core config, with the SAME @Kernel verbs as
// the native f16 path. The accumulator is bf16 too (all three tiles one tier).
//
// Gated only on a working Vulkan device (NOT coopMatrixAvailable) — the whole
// point is that the software path needs no hardware config. Also asserts the
// `note: [mma-tiering]` appraisal fires during lowering. Exact integer check
// (A in 0..4, B in 0..3: products <= 12, 16-term sum <= 192 < 256 — exact in bf16).
const char* kBf16SoftwareSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bmatmul(Buffer<bfloat16> a, Buffer<bfloat16> b,\n"
    "                               Buffer<bfloat16> c) {\n"
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

TEST(XpuCooperativeMatrixDeviceTests, bf16SoftwareMatmulOnDevice) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kBf16SoftwareSource);
    auto k = findMethod(module->getStructures()["test.M"], "bmatmul");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_bf16sw_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);

    // The software tier emits a sticky `note: [mma-tiering]` (not a warning) —
    // capture stderr over lowering and assert the appraisal fires.
    std::ostringstream captured;
    std::streambuf* prev = std::cerr.rdbuf(captured.rdbuf());
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::cerr.rdbuf(prev);
    EXPECT_NE(captured.str().find("[mma-tiering]"), std::string::npos)
        << "software-tier CooperativeMatrix should emit a note; got: "
        << captured.str();

    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

    std::vector<uint16_t> hostA(TILE), hostB(TILE);
    std::vector<uint16_t> ref(TILE, 0);
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
            ref[i * N + j] = f2bf16(acc);
        }

    VulkanDriver vk;
    if (!vk.init()) GTEST_SKIP() << "no working Vulkan device";
    auto dA = vk.alloc(TILE * sizeof(uint16_t));
    auto dB = vk.alloc(TILE * sizeof(uint16_t));
    auto dC = vk.alloc(TILE * sizeof(uint16_t));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), TILE * sizeof(uint16_t)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), TILE * sizeof(uint16_t)));
    std::vector<uint16_t> seed(TILE, 0xFFFFu);
    ASSERT_TRUE(vk.upload(dC, seed.data(), TILE * sizeof(uint16_t)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "bmatmul", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<uint16_t> out(TILE, 0u);
    ASSERT_TRUE(vk.download(out.data(), dC, TILE * sizeof(uint16_t)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "bf16 software tile mismatch at (" << i << "," << j << ")";
}

// LDS-staged GEMM on the Vulkan device: stage the A/B tiles into workgroup-shared
// (Workgroup storage) via CoopStage.panel, barrier, then load the cooperative-
// matrix operands FROM the shared tiles and mma. Exercises OpCooperativeMatrixLoad
// from a Workgroup pointer end-to-end, relying on the two fork SPIR-V backend
// fixes (array-global typing + cooperative-matrix aggregate-pointer access chain).
// Same exact-integer non-uniform check as the global-source path.
const char* kStagedSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.CooperativeMatrix;\n"
    "import cajeta.gpu.core.CoopStage;\n"
    "import cajeta.gpu.core.Barrier;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void staged(Buffer<float16> a, Buffer<float16> b,\n"
    "                              Buffer<float32> c) {\n"
    "        Shared<float16> sa = shared float16[16 * 16];\n"
    "        Shared<float16> sb = shared float16[16 * 16];\n"
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

TEST(XpuCooperativeMatrixDeviceTests, ldsStagedMatmulOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kStagedSource);
    auto k = findMethod(module->getStructures()["test.M"], "staged");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_staged_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, dev);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    ASSERT_FALSE(spirv.empty());

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

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(TILE * sizeof(_Float16));
    auto dB = vk.alloc(TILE * sizeof(_Float16));
    auto dC = vk.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> seed(TILE, -1.0f);
    ASSERT_TRUE(vk.upload(dC, seed.data(), TILE * sizeof(float)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "staged", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<float> out(TILE, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, TILE * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "LDS-staged tile mismatch at (" << i << "," << j << ")";
}

// ---------------------------------------------------------------------------
// Impl-layer / degrade framework (inc-4 brick #4): the explicit
// CAJETA_GPU_<FEATURE>_IMPL override, dogfooded on a SECOND consumer (coop-matrix)
// beyond the AS noun — proving the degrade override is generic, not AS-specific.

// resolveImplTier precedence (host-only, no device): the env override wins when
// set ("software" → Portable, "native" → Native); unset or unknown keeps the
// per-backend base. A unique feature tag ("TESTONLY") keeps this isolated from
// the COOPMATRIX / AS overrides in the same test process.
TEST(ImplTierOverride, resolveImplTierPrecedence) {
    using cajeta::xpu::resolveImplTier;
    using ImplTier = cajeta::xpu::LoweringTarget::ImplTier;
    const char* var = "CAJETA_GPU_TESTONLY_IMPL";

    unsetenv(var);
    EXPECT_EQ(ImplTier::Native,   resolveImplTier("TESTONLY", ImplTier::Native));
    EXPECT_EQ(ImplTier::Portable, resolveImplTier("TESTONLY", ImplTier::Portable));
    {
        EnvGuard g(var, "software");
        EXPECT_EQ(ImplTier::Portable, resolveImplTier("TESTONLY", ImplTier::Native));
        EXPECT_EQ(ImplTier::Portable, resolveImplTier("TESTONLY", ImplTier::Portable));
    }
    {
        EnvGuard g(var, "native");
        EXPECT_EQ(ImplTier::Native, resolveImplTier("TESTONLY", ImplTier::Portable));
        EXPECT_EQ(ImplTier::Native, resolveImplTier("TESTONLY", ImplTier::Native));
    }
    {
        EnvGuard g(var, "garbage");           // unknown value → keep base
        EXPECT_EQ(ImplTier::Native,   resolveImplTier("TESTONLY", ImplTier::Native));
        EXPECT_EQ(ImplTier::Portable, resolveImplTier("TESTONLY", ImplTier::Portable));
    }
    EXPECT_EQ(nullptr, std::getenv(var)) << "env must be restored after the guards";
}

// Forced-software coop-matrix on a NATIVE-capable device. Gated on
// coopMatrixAvailable() — the point is to force the portable tile where the
// hardware DOES have a native config (an unconditional software box would prove
// nothing). With CAJETA_GPU_COOPMATRIX_IMPL=software the f16 `matmul` kernel must:
//   (a) take the portable path — the `note: [mma-tiering]` appraisal fires during
//       lowering (on this device, the un-forced sibling mixedPrecisionMatmulOnDevice
//       does NOT note), and
//   (b) still compute the same result. Inputs are small integers exact in half and
//       the 16-term sum (<=192) is exact in the portable f32 accumulator, so the
//       forced-software tile equals the integer reference to the bit — the same
//       reference the native path matches.
TEST(ImplTierOverride, forcedSoftwareCoopMatrixOnDevice) {
    if (!VulkanDriver::coopMatrixAvailable()) {
        GTEST_SKIP() << "no Vulkan device with a 16x16x16 f16/f16->f32 "
                        "cooperative-matrix config to force off";
    }

    Compiler compiler;
    auto module = compileForInspection(compiler, kMatmulSource);
    auto k = findMethod(module->getStructures()["test.M"], "matmul");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_coopmat_forcedsw_vkdevice", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);

    // Force the portable tier for the tier-selection that happens during
    // lowerKernel, and capture stderr to confirm the appraisal fires.
    std::vector<uint8_t> spirv;
    std::ostringstream captured;
    {
        EnvGuard g("CAJETA_GPU_COOPMATRIX_IMPL", "software");
        std::streambuf* prev = std::cerr.rdbuf(captured.rdbuf());
        cajeta::xpu::vulkan::lowerKernel(k, dev);
        std::cerr.rdbuf(prev);
        spirv = cajeta::xpu::vulkan::emitSpirv(dev, *tm);
    }
    EXPECT_NE(captured.str().find("[mma-tiering]"), std::string::npos)
        << "forced-software CooperativeMatrix should emit the degrade note; got: "
        << captured.str();
    ASSERT_FALSE(spirv.empty());

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

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    auto dA = vk.alloc(TILE * sizeof(_Float16));
    auto dB = vk.alloc(TILE * sizeof(_Float16));
    auto dC = vk.alloc(TILE * sizeof(float));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dB, 0u);
    ASSERT_NE(dC, 0u);
    ASSERT_TRUE(vk.upload(dA, hostA.data(), TILE * sizeof(_Float16)));
    ASSERT_TRUE(vk.upload(dB, hostB.data(), TILE * sizeof(_Float16)));
    std::vector<float> zero(TILE, -1.0f);
    ASSERT_TRUE(vk.upload(dC, zero.data(), TILE * sizeof(float)));

    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "matmul", {dA, dB, dC},
                          /*groupCountX=*/1));

    std::vector<float> out(TILE, -2.0f);
    ASSERT_TRUE(vk.download(out.data(), dC, TILE * sizeof(float)));
    vk.free(dA);
    vk.free(dB);
    vk.free(dC);

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(out[i * N + j], ref[i * N + j])
                << "forced-software tile mismatch at (" << i << "," << j << ")";
}
