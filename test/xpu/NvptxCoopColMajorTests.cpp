//
// NvptxCoopColMajorTests — col-major cooperative-matrix operands on NVPTX
// (xpu-kernel-adaptor plan, Unit 4A).
//
// Why this exists. `requireRowMajor` rejected every non-row-major operand
// with "NVPTX native cooperative matrix (v1) supports only row-major
// operands (layout 0)". That refused 63 kernels in cajeta-llm, measured
// 2026-09-19, and it was OUR limit rather than the hardware's: NVIDIA
// WMMA takes col-major fragments and the fork ships every `_col_stride`
// intrinsic (load_a/b/c, store_d, f16/bf16/s8/u8).
//
// Of 1121 coop load/store sites in those kernels, 739 pass layout 1 and
// ZERO pass a non-constant layout, so the fix is the col intrinsics and
// not bind-time specialization of the layout.
//
// The correctness trap this file guards. A fragment's layout is encoded in
// BOTH the load and the multiply: wmma.mma comes in row.row, row.col,
// col.row and col.col forms. Selecting a col load while leaving the
// multiply at row.row lowers cleanly and computes the WRONG product,
// which is worse than the refusal it replaces. So `mmaPairsTheLoadedLayouts`
// is the load-bearing test here, not the two that assert a clean compile.
//
// These are lowering tests, not device tests: the kernel is lowered during
// compilation, so no GPU is required to learn whether it lowered or which
// instruction it selected.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "../PortableEnv.h"   // portable setenv/unsetenv on MinGW

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <cstdlib>
#include <sstream>
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

// A col-major STORE of the accumulator. A store never feeds `mma`, so this
// arm needs no layout pairing.
const char* kColStoreSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void cs(KernelBuffer<float32> c,\n"
    "            KernelBuffer<float16> a, KernelBuffer<float16> b) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 1, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// A col-major B LOAD, which is the shape every q4k/q6k wmma kernel uses
// (`mb.load(kBase, 1, 256)`). This arm also needs the multiply to become
// row.col.
const char* kColLoadSource =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void cl(KernelBuffer<float32> c,\n"
    "            KernelBuffer<float16> a, KernelBuffer<float16> b) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// Lower `kernelName` for sm_89 and return its PTX, empty with the reason in
// `why`. Shared probe (KernelLoweringProbe.h) behind this file's shape.
std::string ptxFor(const std::string& source, const std::string& kernelName,
                   std::string* why) {
    const Lowered l = lowerForNvptx(source, kernelName, "test.M", "sm_89",
                                    "colmajor");
    if (!l.ok) { *why = l.why; return {}; }
    return l.ptx;
}

} // namespace

// 4A.1.1 — the store half, 496 of the 739 col-major sites.
TEST(NvptxCoopColMajorTests, colMajorStoreLowers) {
    std::string why;
    const std::string ptx = ptxFor(kColStoreSource, "cs", &why);
    ASSERT_FALSE(ptx.empty()) << "col-major store still refused: " << why;
    EXPECT_NE(ptx.find("wmma.store.d.sync.aligned.col"), std::string::npos)
        << "store did not select the col variant:\n" << ptx;
}

// 4A.1.2 — the operand-load half.
TEST(NvptxCoopColMajorTests, colMajorOperandLoadLowers) {
    std::string why;
    const std::string ptx = ptxFor(kColLoadSource, "cl", &why);
    ASSERT_FALSE(ptx.empty()) << "col-major operand load still refused: " << why;
    EXPECT_NE(ptx.find("wmma.load.b.sync.aligned.col"), std::string::npos)
        << "B did not select the col variant:\n" << ptx;
}

// 4A.1.3 — the load-bearing one. A col-major B must pair with a row.col
// multiply. A row.row multiply beside a col-major B lowers cleanly and
// computes the wrong product, so assert on the selected instruction rather
// than on the absence of a diagnostic.
TEST(NvptxCoopColMajorTests, mmaPairsTheLoadedLayouts) {
    std::string why;
    const std::string ptx = ptxFor(kColLoadSource, "cl", &why);
    ASSERT_FALSE(ptx.empty()) << why;
    EXPECT_NE(ptx.find("wmma.mma.sync.aligned.row.col"), std::string::npos)
        << "the multiply did not pair A=row with B=col:\n" << ptx;
    EXPECT_EQ(ptx.find("wmma.mma.sync.aligned.row.row"), std::string::npos)
        << "a row.row multiply survived beside a col-major B:\n" << ptx;
}

// 4A.1.3, the does-NOT-fire half: an all-row kernel keeps selecting row.row,
// so the change is additive rather than a blanket switch.
TEST(NvptxCoopColMajorTests, rowMajorKernelIsUnchanged) {
    std::string why;
    const std::string ptx = ptxFor(kColStoreSource, "cs", &why);
    ASSERT_FALSE(ptx.empty()) << why;
    EXPECT_NE(ptx.find("wmma.mma.sync.aligned.row.row"), std::string::npos)
        << "an all-row-major multiply stopped selecting row.row:\n" << ptx;
    EXPECT_NE(ptx.find("wmma.load.b.sync.aligned.row"), std::string::npos)
        << "a row-major B stopped selecting the row variant:\n" << ptx;
}

namespace {

// ---- 4A.1.4: the numerics gate ------------------------------------------
//
// Everything above asserts which INSTRUCTION was selected. That is a proxy.
// A mispaired layout selects a legal instruction and computes the wrong
// product, so the only thing that closes Unit 4A is running it.
//
// The oracle is self-checking and needs no tolerance. The SAME logical B is
// written twice -- row-major into one buffer, col-major into another -- and
// multiplied by the same A. Row-major B through a row.row multiply and
// col-major B through a row.col multiply must agree, with each other and
// with the host reference. Inputs are small exact integers in f16, so f32
// accumulation is exact and the comparison is EQ rather than NEAR.

const char* kBothLayoutsSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mmRow(KernelBuffer<float16> a,\n"
    "            KernelBuffer<float16> b, KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 0, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void mmCol(KernelBuffer<float16> a,\n"
    "            KernelBuffer<float16> b, KernelBuffer<float32> c) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, 16);\n"
    "    }\n"
    "}\n";

// Lower `kernelName`, assemble for sm_89 and run it on the device with
// (a, b) in and c out. Returns false with `why` set if any stage refused.
bool runTile(const std::string& kernelName, const std::vector<_Float16>& a,
             const std::vector<_Float16>& b, std::vector<float>* out,
             std::string* why) {
    const std::string ptx = ptxFor(kBothLayoutsSource, kernelName, why);
    if (ptx.empty()) return false;
    std::vector<uint8_t> cubin = cajeta::xpu::nvidia::assembleCubin(ptx, "sm_89");
    if (cubin.empty()) { *why = "ptxas rejected the PTX"; return false; }

    cajeta::xpu::nvidia::CudaDriver cuda;
    if (!cuda.init()) { *why = "cuda init failed"; return false; }
    auto mod = cuda.loadModule(cubin.data(), cubin.size());
    if (!mod) { *why = "loadModule failed"; return false; }
    auto fn = cuda.getFunction(mod, kernelName.c_str());
    if (!fn) { *why = "getFunction failed"; return false; }

    auto dA = cuda.alloc(TILE * sizeof(_Float16));
    auto dB = cuda.alloc(TILE * sizeof(_Float16));
    auto dC = cuda.alloc(TILE * sizeof(float));
    cuda.memcpyHtoD(dA, (void*) a.data(), TILE * sizeof(_Float16));
    cuda.memcpyHtoD(dB, (void*) b.data(), TILE * sizeof(_Float16));
    std::vector<float> seed(TILE, -1.0f);
    cuda.memcpyHtoD(dC, seed.data(), TILE * sizeof(float));
    void* params[] = { &dA, &dB, &dC };
    bool ok = cuda.launch(fn, /*grid=*/1, /*block=*/32, params) // a full warp
              && cuda.synchronize();
    out->assign(TILE, -2.0f);
    if (ok) ok = cuda.memcpyDtoH(out->data(), dC, TILE * sizeof(float));
    cuda.free(dA); cuda.free(dB); cuda.free(dC);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

} // namespace

// 4A.1.4 — the product, on the device. A col-major B must give the same
// answer as the same logical B stored row-major, and both must equal the
// host reference.
TEST(NvptxCoopColMajorTests, colMajorProductMatchesRowMajorAndHostOnDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available())
        GTEST_SKIP() << "no CUDA device/driver available";

    std::vector<_Float16> hostA(TILE), bRow(TILE), bCol(TILE);
    std::vector<float> ref(TILE, 0.0f);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (_Float16) ((i + 2 * kk) % 5);
    // ONE logical B, written two ways. i,j-distinct so a transposed read
    // cannot coincidentally match.
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j) {
            float v = (float) ((3 * kk + j) % 4);
            bRow[kk * N + j] = (_Float16) v;   // row-major: B[k][j] at k*16+j
            bCol[j * N + kk] = (_Float16) v;   // col-major: B[k][j] at j*16+k
        }
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            float acc = 0.0f;
            for (unsigned kk = 0; kk < N; ++kk)
                acc += (float) hostA[i * N + kk] * (float) bRow[kk * N + j];
            ref[i * N + j] = acc;
        }

    std::string why;
    std::vector<float> outRow, outCol;
    ASSERT_TRUE(runTile("mmRow", hostA, bRow, &outRow, &why)) << why;
    ASSERT_TRUE(runTile("mmCol", hostA, bCol, &outCol, &why)) << why;

    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j) {
            EXPECT_EQ(outRow[i * N + j], ref[i * N + j])
                << "row-major control disagrees with host at ("
                << i << "," << j << ")";
            EXPECT_EQ(outCol[i * N + j], ref[i * N + j])
                << "COL-MAJOR product is wrong at (" << i << "," << j
                << "): the mma layout pairing is not right";
        }
}

// 4A.1.4, the portable-tier half. The same col-major kernel forced onto the
// software tile must agree with the native tensor-core path element for
// element. This is the comparison the plan item actually names, and it is
// what proves the native fast path did not change the answer.
TEST(NvptxCoopColMajorTests, colMajorNativeAgreesWithPortableTierOnDevice) {
    if (!cajeta::xpu::nvidia::CudaDriver::available())
        GTEST_SKIP() << "no CUDA device/driver available";

    std::vector<_Float16> hostA(TILE), bCol(TILE);
    for (unsigned i = 0; i < N; ++i)
        for (unsigned kk = 0; kk < N; ++kk)
            hostA[i * N + kk] = (_Float16) ((i + 2 * kk) % 5);
    for (unsigned kk = 0; kk < N; ++kk)
        for (unsigned j = 0; j < N; ++j)
            bCol[j * N + kk] = (_Float16) ((3 * kk + j) % 4);

    std::string why;
    std::vector<float> nativeOut, portableOut;
    ASSERT_TRUE(runTile("mmCol", hostA, bCol, &nativeOut, &why)) << why;
    {
        EnvGuard forceSw("CAJETA_GPU_COOPMATRIX_IMPL", "software");
        ASSERT_TRUE(runTile("mmCol", hostA, bCol, &portableOut, &why))
            << "the portable tier refused a col-major operand: " << why;
    }
    for (unsigned i = 0; i < N; ++i)
        for (unsigned j = 0; j < N; ++j)
            EXPECT_EQ(nativeOut[i * N + j], portableOut[i * N + j])
                << "native and portable tiers disagree at ("
                << i << "," << j << ")";
}
