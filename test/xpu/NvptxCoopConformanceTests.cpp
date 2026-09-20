//
// CONFORMANCE: every CooperativeMatrix verb, every ARGUMENT SHAPE, on the
// device, compared against the contract computed in tile coordinates.
//
// WHY THIS FILE EXISTS. NvptxCoopEpilogueTests covered the epilogue and was
// green for a day while the epilogue was wrong. It drove `rank1Accum` and
// `scaledAccumInto`, whose column factor is a Shared VECTOR the lowering
// indexes by the element's own column. The `...S` forms take a register
// instead, through a different branch of the same function, and no test drove
// that branch. On NVIDIA it applied one lane's scalar to eight elements
// spanning four columns: 240 of 256 cells wrong, eleven cajeta-llm kernels
// failing, and a day spent on an innocent `fromWords`.
//
// The lesson is not "write more tests". It is that a verb's ARGUMENT SHAPE is
// a separate code path with a separate chance of encoding one vendor's
// fragment layout, so coverage has to be counted per shape, not per verb.
//
// SO THIS FILE COUNTS. `everyVerbHasAConformanceCase` reads
// CooperativeMatrix.cajeta, extracts the verbs the language actually
// declares, and fails if one is neither exercised here nor explicitly
// delegated to a named sibling suite. Adding a verb without a numeric case
// breaks the build rather than quietly widening the gap.
//
// THE REFERENCE IS THE CONTRACT, NOT THE OTHER TIER. Each case states what
// the tile must hold at (r, c) as arithmetic on r and c — the same sentence
// the stdlib doc makes — so a backend cannot pass by agreeing with its own
// mistake. rowF[r] = 1+r and colF[c] = 1+c are both ramps, so a transposed,
// rotated or broadcast mapping lands on a different number in almost every
// cell, and the failure prints which column's factor actually arrived.
//
// Every product stays under 2^24 (max 3*16*16 = 768), so f32 equality is
// exact and the comparison is EQ rather than a tolerance.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"

#include "cajeta/xpu/nvidia/CudaDriver.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

constexpr unsigned N = 16;
constexpr unsigned TILE = N * N;

// One kernel per argument shape. The preamble is shared; only the tile
// declarations and the verb call differ, so a reader can see the shape
// difference and nothing else.
std::string kernelFor(const char* name, const char* decls, const char* call,
                      bool intOut) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void ";
    s += name;
    s += "(KernelBuffer<float32> rowIn,\n"
         "            KernelBuffer<float32> colIn, KernelBuffer<";
    s += intOut ? "int32" : "float32";
    s += "> out) {\n"
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> colF = shared float32[16];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            colF[lane] = colIn[lane];\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        float32 cv = colIn[(int64) (lane & 15)];\n";
    s += decls;
    s += call;
    s += "    }\n"
         "    public static int32 run() { return 1; }\n"
         "}\n";
    return s;
}

// `Lowered` is the probe header's — same fields, one definition.
Lowered lower(const std::string& src, const std::string& kernel) {
    return lowerForNvptx(src.c_str(), kernel, "test.M", "sm_89", "coopconf");
}

// The tile is read back raw; callers reinterpret. `out` is TILE words.
bool runTile(const std::string& src, const std::string& kernelName,
             const std::vector<float>& rowF, const std::vector<float>& colF,
             std::vector<uint32_t>* out, std::string* why) {
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
    auto dOut = cuda.alloc(TILE * sizeof(uint32_t));
    cuda.memcpyHtoD(dRow, (void*) rowF.data(), N * sizeof(float));
    cuda.memcpyHtoD(dCol, (void*) colF.data(), N * sizeof(float));
    void* params[] = { &dRow, &dCol, &dOut };
    bool ok = cuda.launch(fn, /*grid=*/1, /*block=*/32, params)   // one warp
              && cuda.synchronize();
    out->assign(TILE, 0xDEADBEEFu);
    if (ok) ok = cuda.memcpyDtoH(out->data(), dOut, TILE * sizeof(uint32_t));
    cuda.free(dRow); cuda.free(dCol); cuda.free(dOut);
    if (!ok) { *why = "launch or copy-back failed"; return false; }
    return true;
}

// A demoted tile would compare the portable tier with itself. Every case
// checks this first — the vacuous green this repo treats as a defect.
::testing::AssertionResult loweredNatively(const std::string& src,
                                           const std::string& kernel) {
    const Lowered l = lower(src, kernel);
    if (!l.ok) return ::testing::AssertionFailure() << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    if (r.shape != FrameShape::None)
        return ::testing::AssertionFailure()
            << kernel << " took the portable software tile (depot="
            << r.depotBytes << " local=" << r.localOps
            << "), so this would test the portable tier against itself";
    return ::testing::AssertionSuccess();
}

struct VerbCase {
    const char* verb;       // the method name, matched against the stdlib
    const char* kernel;     // the @Kernel's name
    std::string source;
    bool intOut;
    // The contract, in tile coordinates: what (r, c) must hold.
    std::function<double(unsigned r, unsigned c)> expect;
};

// rowF[r] = 1+r, colF[c] = 1+c everywhere below.
const std::vector<VerbCase>& cases() {
    static const std::vector<VerbCase> v = [] {
        std::vector<VerbCase> cs;

        // facc[r][c] += rowF[r] * colF[c]
        cs.push_back({"rank1Accum", "vR1",
            kernelFor("vR1",
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        facc.rank1Accum(rowF, colF);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return (1.0 + r) * (1.0 + c); }});

        // Same contract, column factor in a register: lane L supplies
        // column L mod 16.
        cs.push_back({"rank1AccumS", "sR1",
            kernelFor("sR1",
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        facc.rank1AccumS(rowF, cv);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return (1.0 + r) * (1.0 + c); }});

        // facc[r][c] += rowF[r] * colF[c] * acc[r][c], acc splatted to 2.
        cs.push_back({"scaledAccumInto", "vSc",
            kernelFor("vSc",
                "        CooperativeMatrix<float32,16,16,2> acc;\n"
                "        acc.splat(2.0f);\n"
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        acc.scaledAccumInto(facc, rowF, colF);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return 2.0 * (1.0 + r) * (1.0 + c); }});

        cs.push_back({"scaledAccumIntoS", "sSc",
            kernelFor("sSc",
                "        CooperativeMatrix<float32,16,16,2> acc;\n"
                "        acc.splat(2.0f);\n"
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        acc.scaledAccumIntoS(facc, rowF, cv);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return 2.0 * (1.0 + r) * (1.0 + c); }});

        // + rowG[r] * colG[c], with G aliased onto F: 2rc + rc = 3rc.
        cs.push_back({"scaledAccumInto2", "vSc2",
            kernelFor("vSc2",
                "        CooperativeMatrix<float32,16,16,2> acc;\n"
                "        acc.splat(2.0f);\n"
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        acc.scaledAccumInto2(facc, rowF, colF, rowF, colF);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return 3.0 * (1.0 + r) * (1.0 + c); }});

        cs.push_back({"scaledAccumInto2S", "sSc2",
            kernelFor("sSc2",
                "        CooperativeMatrix<float32,16,16,2> acc;\n"
                "        acc.splat(2.0f);\n"
                "        CooperativeMatrix<float32,16,16,2> facc;\n"
                "        facc.splat(0.0f);\n",
                "        acc.scaledAccumInto2S(facc, rowF, cv, rowF, cv);\n"
                "        facc.store(out, 0, 0, 16);\n", false),
            false,
            [](unsigned r, unsigned c) { return 3.0 * (1.0 + r) * (1.0 + c); }});

        // iacc[r][c] += colS(c) * this[r][c], receiver splatted to 3.
        // THE CASE THAT WAS NEVER WRITTEN. Its lowering carried a comment
        // saying it "needs NO layout at all", which was true of the
        // accumulator mapping and then applied to the column factor.
        cs.push_back({"scaledAccumI32", "iSc",
            kernelFor("iSc",
                "        int32 cs = (int32) cv;\n"
                "        CooperativeMatrix<int32,16,16,2> mc;\n"
                "        mc.splat(3);\n"
                "        CooperativeMatrix<int32,16,16,2> iacc;\n"
                "        iacc.splat(0);\n",
                "        mc.scaledAccumI32(iacc, cs);\n"
                "        iacc.store(out, 0, 0, 16);\n", true),
            true,
            [](unsigned, unsigned c) { return 3.0 * (1.0 + c); }});

        return cs;
    }();
    return v;
}

// Verbs whose numeric device coverage lives in a named sibling suite. A
// pointer, not an excuse: each names the file that does the work.
const std::map<std::string, std::string>& coveredElsewhere() {
    static const std::map<std::string, std::string> m = {
        {"load",      "XpuCooperativeMatrixDeviceTests + NvptxCoopColMajorTests"},
        {"store",     "XpuCooperativeMatrixDeviceTests + NvptxCoopColMajorTests"},
        {"splat",     "XpuCooperativeMatrixDeviceTests"},
        {"mma",       "XpuCooperativeMatrixDeviceTests + NvptxCoopColMajorTests"},
        {"fromWords", "NvptxCoopFromWordsTests (layout, 8 waves, refusal)"},
    };
    return m;
}

// The verbs the language actually declares, read off the stdlib rather than
// listed here — a list would drift the moment someone adds one.
std::set<std::string> declaredVerbs(std::string* why) {
    std::set<std::string> out;
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (!root) { *why = "CAJETA_SOURCE_ROOT unset"; return out; }
    std::string path = std::string(root)
                     + "/runtime/src/cajeta/xpu/CooperativeMatrix.cajeta";
    std::ifstream in(path);
    if (!in) { *why = "cannot read " + path; return out; }
    std::string line;
    while (std::getline(in, line)) {
        // `    public void name(` — the intrinsic declaration shape.
        const std::string tag = "public void ";
        size_t at = line.find(tag);
        if (at == std::string::npos) continue;
        size_t b = at + tag.size();
        size_t e = line.find('(', b);
        if (e == std::string::npos) continue;
        std::string name = line.substr(b, e - b);
        if (!name.empty()) out.insert(name);
    }
    if (out.empty()) *why = "no `public void` declarations found in " + path;
    return out;
}

} // namespace

// THE HARNESS. Adding a verb to CooperativeMatrix.cajeta without a numeric
// case here fails this test by name.
TEST(NvptxCoopConformance, everyVerbHasAConformanceCase) {
    std::string why;
    std::set<std::string> declared = declaredVerbs(&why);
    ASSERT_FALSE(declared.empty()) << why;

    std::set<std::string> covered;
    for (const VerbCase& c : cases()) covered.insert(c.verb);

    for (const std::string& v : declared) {
        if (covered.count(v)) continue;
        auto it = coveredElsewhere().find(v);
        if (it != coveredElsewhere().end()) continue;
        ADD_FAILURE()
            << "CooperativeMatrix." << v << " has no numeric device case.\n"
            << "Add one to cases() in this file, or — if a sibling suite "
               "already checks it cell by cell on a device — add it to "
               "coveredElsewhere() naming that suite.\n"
            << "Counting coverage per ARGUMENT SHAPE rather than per verb is "
               "the whole point: the scalar-column forms went a day with a "
               "wrong lowering because the vector forms next to them passed.";
    }

    // And the other direction: a case for a verb that no longer exists is
    // dead weight that reads as coverage.
    for (const VerbCase& c : cases())
        EXPECT_TRUE(declared.count(c.verb))
            << "case for CooperativeMatrix." << c.verb
            << ", which the stdlib no longer declares";
    for (auto& kv : coveredElsewhere())
        EXPECT_TRUE(declared.count(kv.first))
            << "coveredElsewhere names CooperativeMatrix." << kv.first
            << ", which the stdlib no longer declares";
}

TEST(NvptxCoopConformance, everyArgumentShapeMatchesTheContract) {
    CAJETA_SKIP_IF_NO_CUDA();

    std::vector<float> rowF(N), colF(N);
    for (unsigned i = 0; i < N; ++i) {
        rowF[i] = 1.0f + (float) i;
        colF[i] = 1.0f + (float) i;
    }

    for (const VerbCase& c : cases()) {
        SCOPED_TRACE(std::string("CooperativeMatrix.") + c.verb);
        ASSERT_TRUE(loweredNatively(c.source, c.kernel));

        std::string why;
        std::vector<uint32_t> raw;
        ASSERT_TRUE(runTile(c.source, c.kernel, rowF, colF, &raw, &why)) << why;

        std::size_t bad = 0;
        for (unsigned r = 0; r < N; ++r)
            for (unsigned col = 0; col < N; ++col) {
                const uint32_t w = raw[r * N + col];
                double have;
                if (c.intOut) have = (double) (int32_t) w;
                else { float f; std::memcpy(&f, &w, 4); have = (double) f; }
                const double want = c.expect(r, col);
                if (have == want) continue;
                if (++bad <= 4) {
                    std::ostringstream extra;
                    // For the ramp inputs, a value that belongs to another
                    // column says WHICH one, which is the whole diagnosis.
                    ADD_FAILURE()
                        << "(" << r << "," << col << ") = " << have
                        << ", want " << want
                        << " — the contract is stated in tile coordinates, so "
                           "a mismatch here means the lowering associated this "
                           "cell with a different row or column";
                }
            }
        EXPECT_EQ(bad, 0u) << bad << " of " << TILE << " cells disagree with "
                           << c.verb << "'s contract";
    }
}
