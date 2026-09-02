//
// XpuCoopFromWordsTests — CooperativeMatrix.fromWords
// (xpu-coopmatrix-fromwords): the register-direct operand feed.
//
// The device test is the LAYOUT verification for the verb: B arrives as
// PACKED NIBBLES (one byte holds two K-values, the Q4_K pairing), each
// lane reads the 8 bytes of its own column, extracts sixteen nibbles into
// four little-endian words and presents them — no LDS, no barrier. The
// result must equal the int32 host reference computed from the widened
// values, with i,j-distinct data so a transposed fragment, a mis-ordered
// word or a swapped nibble cannot match.
//
// The compile-level half pins the tier rule: native on amdgpu (no skip,
// no tier note); the SPIR-V native fragment is opaque and the software
// tile has no lane mapping, so both REJECT loudly with the NATIVE-ONLY
// diagnostic — the scaledAccumIntoS precedent, never a silent demote.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

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
#include <string>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta_test::CajetaJit;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;

// B as packed nibbles, column-major per lane: byte (j*8 + t) holds K-value
// 2t in its LOW nibble and 2t+1 in its HIGH nibble, biased by +8 so the
// signed range -8..7 round-trips through 4 bits. The kernel un-biases.
const char* kFromWordsSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void fw(KernelBuffer<int8> a, KernelBuffer<int8> pb,\n"
    "                          KernelBuffer<int32> c) {\n"
    "        uint32 lane = KernelThread.x();\n"
    "        uint32 col = lane % 16;\n"
    "        int32 w0 = 0; int32 w1 = 0; int32 w2 = 0; int32 w3 = 0;\n"
    "        int32 t = 0;\n"
    "        while (t < 8) {\n"
    "            int32 pbv = (int32) pb[(int64) (col * 8 + (uint32) t)] & 255;\n"
    "            int32 lo = (pbv & 15) - 8;\n"
    "            int32 hi = ((pbv >> 4) & 15) - 8;\n"
    "            int32 k0 = 2 * t;\n"
    "            int32 word = k0 / 4;\n"
    "            int32 sh = (k0 % 4) * 8;\n"
    "            int32 pair = (lo & 255) | ((hi & 255) << 8);\n"
    "            if (word == 0) { w0 = w0 | (pair << sh); }\n"
    "            else if (word == 1) { w1 = w1 | (pair << sh); }\n"
    "            else if (word == 2) { w2 = w2 | (pair << sh); }\n"
    "            else { w3 = w3 | (pair << sh); }\n"
    "            t = t + 1;\n"
    "        }\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.fromWords(w0, w1, w2, w3);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.splat(0);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_fromwords_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_fromwords_arch_" + std::to_string(rng()));
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

int32_t runI32On(cajeta::xpu::Backend be, const std::string& src,
                 std::string* errOut) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.M", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    *errOut = testing::internal::GetCapturedStderr();
    return r;
}

} // namespace

// The device exactness ride: B fed from packed nibbles through fromWords
// equals the widened host reference, every element.
TEST(XpuCoopFromWordsTests, packedNibbleBFeedMatchesHostOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no ROCm/HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kFromWordsSource);
    auto k = findMethod(module->getStructures()["test.M"], "fw");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_fromwords_amddevice", ctx);
    configureDeviceModule(dev, *tm);
    lowerKernel(k, dev);
    std::vector<uint8_t> hsaco = assembleHsaco(dev, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    // A: signed, i,k-distinct. B (logical, [k][j]): signed -8..7,
    // k,j-distinct, then PACKED column-major two-per-byte.
    std::vector<int8_t> hostA(TILE);
    std::vector<int32_t> logicalB(TILE);
    std::vector<int8_t> packedB(N * 8);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (int8_t) ((int) ((i + 2 * kk) % 5) - 2);
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            logicalB[kk * N + j] = (int) ((3 * kk + 5 * j) % 16) - 8;
    for (unsigned j = 0; j < N; ++j)
        for (unsigned t = 0; t < 8; ++t) {
            int lo = logicalB[(2 * t) * N + j] + 8;
            int hi = logicalB[(2 * t + 1) * N + j] + 8;
            packedB[j * 8 + t] = (int8_t) ((lo & 15) | ((hi & 15) << 4));
        }
    std::vector<int32_t> ref(TILE, 0);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            int32_t acc = 0;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += (int32_t) hostA[i * N + kk] * logicalB[kk * N + j];
            ref[i * N + j] = acc;
        }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "fw");
    ASSERT_NE(fn, nullptr);

    HipDevicePtr dA = hip.alloc(TILE);
    HipDevicePtr dB = hip.alloc(N * 8);
    HipDevicePtr dC = hip.alloc(TILE * sizeof(int32_t));
    ASSERT_NE(dA, nullptr);
    ASSERT_NE(dB, nullptr);
    ASSERT_NE(dC, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dA, hostA.data(), TILE));
    ASSERT_TRUE(hip.memcpyHtoD(dB, packedB.data(), N * 8));
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
                ADD_FAILURE() << "fromWords mismatch at (" << i << "," << j
                              << "): got " << out[i * N + j] << " want "
                              << ref[i * N + j];
    EXPECT_EQ(mismatches, 0u) << "fromWords tile had " << mismatches
                              << " mismatches";
}

// Native on amdgpu: no skip, no tier note.
TEST(XpuCoopFromWordsTests, lowersNativelyOnAmdgpu) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Amdgpu, kFromWordsSource,
                       &err), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "fromWords must lower on amdgpu, not skip:\n" << err;
    EXPECT_EQ(err.find("[mma-tiering]"), std::string::npos)
        << "int8 + fromWords is native on amdgpu:\n" << err;
}

// The SPIR-V cooperative matrix is opaque: no per-lane fragment to build,
// so the kernel is REJECTED with the NATIVE-ONLY diagnostic.
TEST(XpuCoopFromWordsTests, rejectsLoudlyOnSpirv) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Spirv, kFromWordsSource,
                       &err), 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "off-native fromWords must SKIP the kernel loudly:\n" << err;
    EXPECT_NE(err.find("NATIVE-ONLY"), std::string::npos)
        << "the skip must carry the NATIVE-ONLY explanation:\n" << err;
}

// The software tile has no lane mapping: same loud rejection on the cpu
// backend.
TEST(XpuCoopFromWordsTests, rejectsLoudlyOnCpu) {
    std::string err;
    EXPECT_EQ(runI32On(cajeta::xpu::Backend::Cpu, kFromWordsSource,
                       &err), 1);
    EXPECT_NE(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the software tile must SKIP the kernel loudly:\n" << err;
    EXPECT_NE(err.find("NATIVE-ONLY"), std::string::npos)
        << "the skip must carry the NATIVE-ONLY explanation:\n" << err;
}
