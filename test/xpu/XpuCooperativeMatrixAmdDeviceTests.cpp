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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmma(Buffer<float16> a, Buffer<float16> b,\n"
    "                            Buffer<float32> c) {\n"
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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wmmabf(Buffer<bfloat16> a, Buffer<bfloat16> b,\n"
    "                              Buffer<float32> c) {\n"
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
