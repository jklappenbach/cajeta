//
// NvptxCoopInt8Tests — int8 cooperative matrices on NVIDIA tensor cores
// (xpu-kernel-adaptor plan, 4A.2.7).
//
// Why this exists. `NvptxKernelLowering::coopMatrixTier` returns Native only
// for an `isFloatTy` accumulator with half/bfloat operands, so
// `CooperativeMatrix<int8,16,16>` with an int32 accumulator falls to the
// portable software tile — and that tile IS the scratch, which is 20 of the
// 21 kernels still on the spill list after 9cce4162. Measured on sm_89,
// 2026-09-19: of the 66 [mma-tiering] fallbacks the llm library emits, 58
// are `CooperativeMatrix<int8,16,16>`. It is the whole prize.
//
// The hardware has had it since sm_72 and the fork ships the full set:
// load.{a,b}.{row,col}[.stride].s8, load.c / store.d .s32, and mma in all
// four layout pairings.
//
// TWO CORRECTNESS TRAPS, both of which lower cleanly and are wrong.
//
// 1. SATURATION. NVIDIA offers `.satfinite` and plain forms. The portable
//    tile accumulates with `CreateMul`/`CreateAdd` carrying NO nsw/nuw
//    (`KernelLowering.cpp`, the software mma loop), i.e. two's-complement
//    WRAPPING. `.satfinite` clamps instead, so it would agree with the
//    portable tier on every input that does not overflow and disagree on
//    the ones that do — the worst possible failure shape, and invisible to
//    a test that does not deliberately overflow. `mmaDoesNotSaturate` and
//    the device test's overflow rows are the guard. Note that 16 products
//    of int8 x int8 cannot overflow i32 on their own (16 * 128 * 128 =
//    262144), so overflow has to be reached through a seeded accumulator,
//    which is what the device test does.
//
// 2. MIXED SIGNEDNESS. PTX has `.s8` and `.u8` but NO mixed form, so A and
//    B must agree. cajeta's `dotAccum` explicitly supports unsigned weights
//    against signed activations, so this combination is expressible. It
//    does not occur today — zero `CooperativeMatrix<uint8` in the llm
//    library against 85 int8 sites — but silently picking one signedness
//    would compute the wrong product (-1 reading as 255). It must refuse,
//    by name, and say what to do instead.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../PortableEnv.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;

struct EnvGuard {
    std::string name;
    EnvGuard(const char* var, const char* value) : name(var) {
        setenv(var, value, /*overwrite=*/1);
    }
    ~EnvGuard() { unsetenv(name.c_str()); }
};

// D[i32] = A[i8] * B[i8] + C[i32], one warp, row x col — the shape the Q4_K
// and MXFP4 MMQ kernels use. `c` is seeded by the caller so the accumulate
// can be driven into overflow.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mmI8(KernelBuffer<int8> a, KernelBuffer<int8> b,\n"
    "            KernelBuffer<int32> c) {\n"
    "        CooperativeMatrix<int8,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.load(c, 0, 0, 16);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// The same kernel with an UNSIGNED A against a signed B. WMMA has no mixed
// form, so this must refuse rather than pick one.
const char* kMixedSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mmMixed(KernelBuffer<uint8> a,\n"
    "            KernelBuffer<int8> b, KernelBuffer<int32> c) {\n"
    "        CooperativeMatrix<uint8,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<int8,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<int32,16,16,2> mc;\n"
    "        mc.load(c, 0, 0, 16);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

Lowered lower(const char* src, const std::string& kernel) {
    return lowerForNvptx(src, kernel, "test.M", "sm_89", "coopi8");
}

} // namespace

// 4A.2.7 — int8 A/B with an int32 accumulator reaches the tensor cores.
TEST(NvptxCoopInt8Tests, int8MmaLowersToWmma) {
    const Lowered l = lower(kSource, "mmI8");
    ASSERT_TRUE(l.ok) << "int8 coop matmul still refuses: " << l.why;
    EXPECT_NE(l.ptx.find("wmma.mma.sync.aligned"), std::string::npos)
        << "no wmma instruction — still on the software tile:\n" << l.ptx;
    EXPECT_NE(l.ptx.find("s32.s8.s8.s32"), std::string::npos)
        << "wmma did not select the s8 form:\n" << l.ptx;
}

// The tile IS the scratch, so going native must take the frame with it.
TEST(NvptxCoopInt8Tests, int8MmaLeavesNoSoftwareTile) {
    const Lowered l = lower(kSource, "mmI8");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None)
        << "depot=" << r.depotBytes << " local=" << r.localOps
        << " — the portable tile is still there";
}

// LOAD-BEARING. `.satfinite` clamps; the portable tile wraps. Selecting the
// saturating form agrees on every non-overflowing input and disagrees on the
// rest, which is the failure shape that hides.
TEST(NvptxCoopInt8Tests, int8MmaDoesNotSaturate) {
    const Lowered l = lower(kSource, "mmI8");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ptx.find("satfinite"), std::string::npos)
        << "the saturating form disagrees with the portable tier on "
           "overflow:\n" << l.ptx;
}

// Mixed signedness has no WMMA form. Refuse by name rather than pick one:
// executing signed int8 as unsigned reads -1 as 255.
TEST(NvptxCoopInt8Tests, mixedSignednessRefusesByName) {
    const Lowered l = lower(kMixedSource, "mmMixed");
    EXPECT_FALSE(l.ok)
        << "mixed-signedness operands must not silently select one form";
    EXPECT_NE(l.why.find("SIGNEDNESS"), std::string::npos)
        << "the refusal must say why, got: " << l.why;
}

// Does NOT fire: matching signedness is the common case and must lower.
// Without this the refusal above could be implemented by refusing everything.
TEST(NvptxCoopInt8Tests, matchingSignednessDoesNotRefuse) {
    const Lowered l = lower(kSource, "mmI8");
    EXPECT_TRUE(l.ok) << "matching signedness must lower: " << l.why;
}

// ---------------------------------------------------------------------------

namespace {

bool runTile(const std::string& kernelName, const std::vector<int8_t>& a,
             const std::vector<int8_t>& b, const std::vector<int32_t>& cSeed,
             std::vector<int32_t>* out, std::string* why) {
    const Lowered l = lower(kSource, kernelName);
    if (!l.ok) { *why = l.why; return false; }
    std::vector<uint8_t> cubin =
        cajeta::xpu::nvidia::assembleCubin(l.ptx, "sm_89");
    if (cubin.empty()) { *why = "ptxas rejected the PTX"; return false; }

    cajeta::xpu::nvidia::CudaDriver cuda;
    if (!cuda.init()) { *why = "cuda init failed"; return false; }
    auto mod = cuda.loadModule(cubin.data(), cubin.size());
    if (!mod) { *why = "loadModule failed"; return false; }
    auto fn = cuda.getFunction(mod, kernelName.c_str());
    if (!fn) { *why = "getFunction failed"; return false; }

    auto dA = cuda.alloc(TILE);
    auto dB = cuda.alloc(TILE);
    auto dC = cuda.alloc(TILE * sizeof(int32_t));
    cuda.memcpyHtoD(dA, (void*) a.data(), TILE);
    cuda.memcpyHtoD(dB, (void*) b.data(), TILE);
    cuda.memcpyHtoD(dC, (void*) cSeed.data(), TILE * sizeof(int32_t));
    void* params[] = { &dA, &dB, &dC };
    bool ok = cuda.launch(fn, /*grid=*/1, /*block=*/32, params)   // one warp
              && cuda.synchronize();
    out->assign(TILE, -1);
    if (ok) ok = cuda.memcpyDtoH(out->data(), dC, TILE * sizeof(int32_t));
    cuda.free(dA); cuda.free(dB); cuda.free(dC);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

} // namespace

// 4A.2.7 acceptance: the tensor cores return exactly what the software tile
// returns, INCLUDING where the accumulate overflows int32. The seed is set
// near INT32_MAX on purpose — 16 products of int8 x int8 cannot overflow on
// their own, so without a seeded accumulator this test would pass against a
// saturating instruction and prove nothing.
TEST(NvptxCoopInt8Tests, int8MmaMatchesThePortableTierIncludingOverflow) {
    CAJETA_SKIP_IF_NO_CUDA();

    std::vector<int8_t> a(TILE), b(TILE);
    std::vector<int32_t> seed(TILE);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned k = 0; k < N; ++k)
            a[i * N + k] = (int8_t) (((int) (i * 7 + k * 13) % 255) - 127);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned j = 0; j < N; ++j)
            b[j * N + k] = (int8_t) (((int) (k * 11 + j * 5) % 255) - 127);
    for (unsigned i = 0; i < TILE; ++i)
        // Half the tile ordinary, half parked where the accumulate must wrap.
        seed[i] = (i % 2 == 0) ? (int32_t) (i * 3)
                               : (int32_t) (INT32_MAX - 1000 + (int) (i % 97));

    std::string why;
    std::vector<int32_t> native, portable;
    ASSERT_TRUE(runTile("mmI8", a, b, seed, &native, &why)) << why;
    {
        EnvGuard forceSw("CAJETA_GPU_COOPMATRIX_IMPL", "software");
        ASSERT_TRUE(runTile("mmI8", a, b, seed, &portable, &why))
            << "the portable tier refused: " << why;
    }

    std::size_t bad = 0, overflowed = 0;
    for (unsigned i = 0; i < TILE; ++i) {
        if (seed[i] > INT32_MAX - 100000 && native[i] < 0) ++overflowed;
        if (native[i] != portable[i]) {
            if (bad < 5)
                ADD_FAILURE() << "lane " << i << " seed=" << seed[i]
                              << " native=" << native[i]
                              << " portable=" << portable[i];
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u) << "tensor cores disagree with the software tile";
    EXPECT_GT(overflowed, 0u)
        << "no lane actually wrapped, so this run did not test saturation";
}
