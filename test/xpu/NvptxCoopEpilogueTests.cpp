//
// NvptxCoopEpilogueTests — the fused GEMM epilogue on NVIDIA
// (xpu-kernel-adaptor plan, 4A.2.8).
//
// Why this exists. `coopMatrixEpilogueSupported()` is false on nvptx, so the
// tier scan demotes EVERY cooperative-matrix tile in any kernel that calls
// `scaledAccumInto`/`rank1Accum` to the portable software tile. That tile is
// the scratch, and it is what holds 7 of the 11 kernels still on the sm_89
// spill list after int8 WMMA landed (4A.2.7). Those 7 are the `WmmaKernel`
// `*Epi*` / `*Mw*` / `*T2*` variants.
//
// WHAT THE EPILOGUE NEEDS THAT `mma` DID NOT. The contract is
//
//     facc[r][c] += rowF[r] * colF[c] * acc[r][c]
//
// per fragment element OF THE CURRENT LANE. To evaluate it a backend must map
// each fragment element to its (row, column). AMD hardcodes its own layout
// (`AmdgpuKernelLowering`: column = lane & 15, row = 2*e + (lane >> 4)), which
// is exactly the per-vendor knowledge this file has to establish for NVIDIA.
//
// A CHEAPER FIX WAS RULED OUT BY MEASUREMENT, not by argument — see plan
// 4A.2.8.0. The same boolean also gates `scaledAccumI32`, whose contract is
// elementwise ("no row/column math") and therefore needs NO layout at all, so
// splitting the capability in two would let nvptx serve that verb on opaque
// fragments today. It buys nothing: of the 20 epilogue-using kernels in
// `WmmaKernel.cajeta`, ZERO use `scaledAccumI32` alone. Every one reaches for
// the row/column family. The layout requirement is real.
//
// THIS FILE IS ALSO THE INSTRUMENT THAT ESTABLISHES THE LAYOUT. I have already
// misdiagnosed this blocker once — I guessed the 24-bit multiply, and reading
// llama.cpp showed the obstacle is the fragment layout — so the mapping is
// MEASURED here rather than asserted from a table.
//
// The trick is that `store` reconstructs the logical matrix through the same
// layout convention the fragment uses, whatever it is. So a WRONG (r,c)
// formula in the epilogue does not produce noise, it produces a READABLE
// PERMUTATION of the intended matrix. Two probes pin it down completely:
//
//     rowProbe:  rowF[r] = r,  colF[c] = 1   =>  facc[r][c] should be r
//     colProbe:  rowF[r] = 1,  colF[c] = c   =>  facc[r][c] should be c
//
// Together they name the (row, column) that every position actually received,
// so a failure prints the true mapping rather than merely reporting a
// mismatch. `epilogueMatchesThePortableTier` is the acceptance gate; the two
// probes are the diagnostic that tells you what to fix when it fails.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../PortableEnv.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;

// Local, as in NvptxCoopInt8Tests and NvptxCoopColMajorTests. That it is now
// copied a fourth time is the same duplication plan 1.5.8.4 tracks for
// compileForInspection; it belongs in the probe header, not here.
struct EnvGuard {
    std::string name;
    EnvGuard(const char* var, const char* value) : name(var) {
        setenv(var, value, /*overwrite=*/1);
    }
    ~EnvGuard() { unsetenv(name.c_str()); }
};

// A zeroed f32 accumulator, one rank-1 update, stored back. Nothing else, so
// a discrepancy can only come from the epilogue's element-to-(r,c) mapping.
const char* kRank1Source =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.WaveVector;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void epiR1(KernelBuffer<float32> rowIn,\n"
    "            KernelBuffer<float32> colIn, KernelBuffer<float32> out) {\n"
    "        Shared<float32> rowF = shared float32[16];\n"
    "        Shared<float32> colF = shared float32[16];\n"
    "        uint32 lane = KernelThread.x();\n"
    "        if (lane < 16) {\n"
    "            rowF[lane] = rowIn[lane];\n"
    "            colF[lane] = colIn[lane];\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        facc.splat(0.0f);\n"
    "        facc.rank1Accum(rowF, colF);\n"
    "        facc.store(out, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// The scaled form, which additionally multiplies by an int32 accumulator the
// kernel just produced. This is the shape the 7 demoted kernels actually use.
const char* kScaledSource =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.WaveVector;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void epiScaled(KernelBuffer<float32> rowIn,\n"
    "            KernelBuffer<float32> colIn, KernelBuffer<float32> out) {\n"
    "        Shared<float32> rowF = shared float32[16];\n"
    "        Shared<float32> colF = shared float32[16];\n"
    "        uint32 lane = KernelThread.x();\n"
    "        if (lane < 16) {\n"
    "            rowF[lane] = rowIn[lane];\n"
    "            colF[lane] = colIn[lane];\n"
    "        }\n"
    "        Barrier.workgroup();\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        acc.splat(1.0f);\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        facc.splat(0.0f);\n"
    "        acc.scaledAccumInto(facc, rowF, colF);\n"
    "        facc.store(out, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

// The SCALAR column form, and the gap that let a wrong epilogue ship for a
// day. Every test above drives `rank1Accum` / `scaledAccumInto`, whose column
// factor is a Shared VECTOR the lowering indexes by the element's own column.
// `rank1AccumS` / `scaledAccumIntoS` / `scaledAccumI32` pass ONE register
// value instead, on the contract (CooperativeMatrix.cajeta:247) that "on the
// native WMMA mapping the column of every element a lane holds is `lane & 15`".
//
// That premise is AMD's. On NVIDIA's m16n16k16 a lane's eight accumulator
// elements span FOUR columns — `col2 + {0, 1, 8, 9}` — so one scalar cannot be
// the factor for all of them, and the first lowering used it for all eight.
// Column 0 came out right and everything else was wrong, which is precisely
// what cajeta-llm's eleven "Mw" kernels reported: element 0 correct, row 1
// onward wrong, on every kernel that used an S-form.
//
// The kernel below passes `colF = 1 + (lane & 15)`, so under the contract
// cell (r, c) must be `1 + c` — the cell names its own column. A scalar
// applied to the whole fragment instead yields `1 + lane`, which is a
// readable permutation rather than a near miss.
const char* kScalarColSource =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.WaveVector;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void epiScalarCol(KernelBuffer<float32> rowIn,\n"
    "            KernelBuffer<float32> colIn, KernelBuffer<float32> out) {\n"
    "        Shared<float32> rowF = shared float32[16];\n"
    "        uint32 lane = KernelThread.x();\n"
    "        if (lane < 16) { rowF[lane] = rowIn[lane]; }\n"
    "        Barrier.workgroup();\n"
    "        float32 cv = colIn[(int64) (lane & 15)];\n"
    "        CooperativeMatrix<float32,16,16,2> facc;\n"
    "        facc.splat(0.0f);\n"
    "        facc.rank1Accum(rowF, WaveVector.ofLane(cv));\n"
    "        facc.store(out, 0, 0, 16);\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

Lowered lower(const char* src, const std::string& kernel) {
    return lowerForNvptx(src, kernel, "test.M", "sm_89", "coopepi");
}

bool runEpilogue(const char* src, const std::string& kernelName,
                 const std::vector<float>& rowF,
                 const std::vector<float>& colF,
                 std::vector<float>* out, std::string* why) {
    const Lowered l = lower(src, kernelName);
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

    auto dRow = cuda.alloc(N * sizeof(float));
    auto dCol = cuda.alloc(N * sizeof(float));
    auto dOut = cuda.alloc(TILE * sizeof(float));
    cuda.memcpyHtoD(dRow, (void*) rowF.data(), N * sizeof(float));
    cuda.memcpyHtoD(dCol, (void*) colF.data(), N * sizeof(float));
    void* params[] = { &dRow, &dCol, &dOut };
    bool ok = cuda.launch(fn, /*grid=*/1, /*block=*/32, params)  // one warp
              && cuda.synchronize();
    out->assign(TILE, -1.0f);
    if (ok) ok = cuda.memcpyDtoH(out->data(), dOut, TILE * sizeof(float));
    cuda.free(dRow); cuda.free(dCol); cuda.free(dOut);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

// LOAD-BEARING, and the reason the first run of this file was worthless.
// While the epilogue has no native lowering, the tier scan demotes the tile
// and "native" IS the portable tile — so every comparison below agrees with
// itself and every probe reads the software tile's (correct) layout. All four
// tests passed on the first run for exactly that reason, which is the vacuous
// green this repo treats as a defect. Every device test calls this first.
::testing::AssertionResult loweredNatively(const char* src,
                                           const std::string& kernel) {
    const Lowered l = lower(src, kernel);
    if (!l.ok) return ::testing::AssertionFailure() << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    if (r.shape != FrameShape::None)
        return ::testing::AssertionFailure()
            << kernel << " took the portable software tile (depot="
            << r.depotBytes << " local=" << r.localOps << "), so a "
               "native-vs-portable comparison would compare the portable "
               "tier with itself and pass without testing anything";
    return ::testing::AssertionSuccess();
}

// Report a wrong mapping as a mapping, not as a list of bad floats. Given the
// two probes, position (r,c) tells us which row and column the epilogue
// actually associated with the fragment element that landed there.
void reportMapping(const std::vector<float>& rowProbe,
                   const std::vector<float>& colProbe) {
    std::string msg =
        "the epilogue's element-to-(row,column) mapping is wrong on nvptx.\n"
        "  position (r,c) -> (row,col) the lowering actually used:\n";
    unsigned shown = 0;
    for (unsigned r = 0; r < N && shown < 8; ++r) {
        for (unsigned c = 0; c < N && shown < 8; ++c) {
            const int gotRow = (int) rowProbe[r * N + c];
            const int gotCol = (int) colProbe[r * N + c];
            if (gotRow != (int) r || gotCol != (int) c) {
                msg += "    (" + std::to_string(r) + "," + std::to_string(c) +
                       ") got (" + std::to_string(gotRow) + "," +
                       std::to_string(gotCol) + ")\n";
                ++shown;
            }
        }
    }
    ADD_FAILURE() << msg;
}

} // namespace

// 4A.2.8, the reason the unit exists: the kernel must not be demoted. A note
// on stderr saying the tiles took the software tile means the 7 spilling
// kernels keep their scratch, which is the whole blocker.
TEST(NvptxCoopEpilogueTests, rank1AccumDoesNotDemoteToTheSoftwareTile) {
    const Lowered l = lower(kRank1Source, "epiR1");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None)
        << "depot=" << r.depotBytes << " local=" << r.localOps
        << " — rank1Accum still demoted the tile to the portable software "
           "path, so the epilogue has no native lowering yet. That is "
           "4A.2.8 and it is what keeps 7 kernels on the spill list.";
}

TEST(NvptxCoopEpilogueTests, scaledAccumIntoDoesNotDemoteToTheSoftwareTile) {
    const Lowered l = lower(kScaledSource, "epiScaled");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None)
        << "depot=" << r.depotBytes << " local=" << r.localOps
        << " — scaledAccumInto still demoted the tile to the portable "
           "software path.";
}

// The layout, measured. Two probes name the row and the column that each
// position actually received, so a wrong formula prints itself.
TEST(NvptxCoopEpilogueTests, theFragmentLayoutPlacesEveryElementAtItsOwnRowAndColumn) {
    CAJETA_SKIP_IF_NO_CUDA();
    ASSERT_TRUE(loweredNatively(kRank1Source, "epiR1"));

    std::vector<float> ramp(N), ones(N, 1.0f);
    for (unsigned i = 0; i < N; ++i) ramp[i] = (float) i;

    std::string why;
    std::vector<float> rowProbe, colProbe;
    ASSERT_TRUE(runEpilogue(kRank1Source, "epiR1", ramp, ones, &rowProbe, &why))
        << why;
    ASSERT_TRUE(runEpilogue(kRank1Source, "epiR1", ones, ramp, &colProbe, &why))
        << why;

    bool bad = false;
    for (unsigned r = 0; r < N && !bad; ++r)
        for (unsigned c = 0; c < N && !bad; ++c)
            if ((int) rowProbe[r * N + c] != (int) r ||
                (int) colProbe[r * N + c] != (int) c)
                bad = true;
    if (bad) reportMapping(rowProbe, colProbe);
}

// The scalar column factor must reach EVERY column of the lane's fragment,
// not just the one the AMD layout would have given it. rowF is all ones and
// colF is `1 + c`, so every cell must equal its own column index plus one.
TEST(NvptxCoopEpilogueTests, theScalarColumnFormReachesEveryColumnOfTheFragment) {
    CAJETA_SKIP_IF_NO_CUDA();
    ASSERT_TRUE(loweredNatively(kScalarColSource, "epiScalarCol"));

    std::vector<float> ones(N, 1.0f), colRamp(N);
    for (unsigned i = 0; i < N; ++i) colRamp[i] = 1.0f + (float) i;

    std::string why;
    std::vector<float> got;
    ASSERT_TRUE(runEpilogue(kScalarColSource, "epiScalarCol", ones, colRamp,
                            &got, &why))
        << why;

    std::size_t bad = 0;
    for (unsigned r = 0; r < N; ++r)
        for (unsigned c = 0; c < N; ++c) {
            const float want = 1.0f + (float) c;
            const float have = got[r * N + c];
            if (have == want) continue;
            if (++bad <= 6)
                ADD_FAILURE()
                    << "(" << r << "," << c << ") = " << have << ", want "
                    << want << "; the value present is column "
                    << (int) (have - 1.0f)
                    << ", so the scalar was taken from the lane that owns THAT"
                       " column instead of this one";
        }
    EXPECT_EQ(bad, 0u) << bad << " of " << TILE << " cells took the wrong "
                          "column's factor";
}

// The acceptance gate. The native epilogue must return exactly what the
// portable tile returns, on values where a transposed or rotated mapping
// cannot hide: rowF and colF are both non-constant and mutually coprime in
// their strides, so every (r,c) product is distinguishable from its neighbours.
TEST(NvptxCoopEpilogueTests, epilogueMatchesThePortableTier) {
    CAJETA_SKIP_IF_NO_CUDA();
    ASSERT_TRUE(loweredNatively(kRank1Source, "epiR1"));

    std::vector<float> rowF(N), colF(N);
    for (unsigned i = 0; i < N; ++i) {
        rowF[i] = 1.0f + (float) i * 3.0f;
        colF[i] = 2.0f + (float) i * 7.0f;
    }

    std::string why;
    std::vector<float> native, portable;
    ASSERT_TRUE(runEpilogue(kRank1Source, "epiR1", rowF, colF, &native, &why))
        << why;
    {
        EnvGuard forceSw("CAJETA_GPU_COOPMATRIX_IMPL", "software");
        ASSERT_TRUE(
            runEpilogue(kRank1Source, "epiR1", rowF, colF, &portable, &why))
            << "the portable tier refused: " << why;
    }

    std::size_t bad = 0;
    for (unsigned i = 0; i < TILE; ++i) {
        if (native[i] != portable[i]) {
            if (bad < 5)
                ADD_FAILURE()
                    << "(" << (i / N) << "," << (i % N) << ") native="
                    << native[i] << " portable=" << portable[i]
                    << " expected=" << (rowF[i / N] * colF[i % N]);
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u) << bad << " of " << TILE
                       << " positions disagree with the software tile";
}
