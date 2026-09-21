//
// WaveVector + ofSlice: the vendor-neutral column vector of the coop-matrix
// epilogue, MEASURED cell by cell on every tier that is here to run it
// (xpu-kernel-adaptor 4A.5.4.C.impl).
//
// The decision retired the S-forms (`scaledAccumIntoS` and friends), whose
// column factor was a per-lane scalar written in a fragment layout, in favour
// of one verb whose column argument is a `WaveVector<T,N>` built three ways:
//
//   ofSlice(arr, base[, stride])  the factors already sit in a Shared array;
//                                 column c reads arr[base + c*stride]
//   broadcast(v)                  the same factor for every column
//   ofLane(v)                     a per-lane register value (native-only)
//
// The STRIDE is the load-bearing new capability and the one a stride-1 slice
// (which the vector form already accepted) cannot express: the real kernels
// pack several tiles' factors into one panel, e.g. cfAll[(wid*16+lane)*8 + j],
// so column c's factor is 8 elements from column c+1's. A test that used
// stride 1 would pass against a lowering that ignored the stride entirely,
// which is exactly the invisible-absence trap this unit exists to close — so
// every case here uses a stride > 1 and POISONS the elements a stride-1 read
// would land on.
//
// rowF[r] = 1+r and (for ofSlice) colF[c] = 1+c are ramps under 2^24, acc is
// splat(2), so facc[r][c] = 2*(1+r)*(1+c) is exact and a wrong stride, a
// transposed mapping or a dropped column lands on a different number.
//
// The guarded case is probe 3's killer shape: the epilogue sits INSIDE a
// wave-divergent guard with a panel-staging barrier. ofSlice adds no
// verb-internal barrier, so the CPU loop-fission has nothing new to refuse and
// the kernel lowers where ofLane's staging barrier did not — asserted here by
// the ABSENCE of a skip note plus correct numerics, not by inspection.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// A kernel exercising one WaveVector shape, its contract, and the bit it owns.
struct Case {
    const char* name;     // human label / the @Kernel name / skip-note key
    const char* body;     // the kernel body (writes `out` at 0,0 stride 16)
    const char* want;     // contract as a cajeta expression in r and c
    bool intOut;          // int32 accumulator/output (scaledAccumI32)
};

const std::vector<Case>& cases() {
    static const std::vector<Case> c = {
        // ofSlice, stride 2, base 0. Column c is at cfSh[2*c]; the odd slots
        // hold a poison value a stride-1 read would pick up on odd columns.
        {"kSlice",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[64];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            cfSh[lane * 2] = cfIn[lane];\n"
         "            cfSh[lane * 2 + 1] = -999.0f;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        acc.scaledAccumInto(facc, rowF, WaveVector.ofSlice(cfSh, 0, 2));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        // ofSlice, stride 4, base 3 — a non-zero base as well as a stride, so
        // the base is not silently dropped either. Column c is at cfSh[3+4*c].
        {"kSliceBase",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[80];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            cfSh[3 + lane * 4] = cfIn[lane];\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        acc.scaledAccumInto(facc, rowF, WaveVector.ofSlice(cfSh, 3, 4));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        // broadcast: colF[c] = 0.5 for every column, so facc[r][c] =
        // (rowF[r]*0.5)*2 = (1+r), independent of c.
        {"kBcast",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) { rowF[lane] = rowIn[lane]; }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        acc.scaledAccumInto(facc, rowF, WaveVector.broadcast(0.5f));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "1.0f + (float32) r", false},

        // rank1Accum via ofSlice: facc[r][c] = rowF[r]*colF[c] = (1+r)(1+c).
        {"kRank1",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[64];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            cfSh[lane * 2] = cfIn[lane];\n"
         "            cfSh[lane * 2 + 1] = -999.0f;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        facc.rank1Accum(rowF, WaveVector.ofSlice(cfSh, 0, 2));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "(1.0f + (float32) r) * (1.0f + (float32) c)", false},

        // scaledAccumInto2 via ofSlice on both columns: facc[r][c] =
        // (rowF*colF)*acc + rowF*colF = (1+r)(1+c)*2 + (1+r)(1+c).
        {"kDual",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[64];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            cfSh[lane * 2] = cfIn[lane];\n"
         "            cfSh[lane * 2 + 1] = -999.0f;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        acc.scaledAccumInto2(facc, rowF, WaveVector.ofSlice(cfSh, 0, 2),\n"
         "                             rowF, WaveVector.ofSlice(cfSh, 0, 2));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "3.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        // A NAMED WaveVector local, then used by the verb — the shape every
        // migrated kernel actually writes (`WaveVector<..> v = ofSlice(..); ..
        // verb(.., v)`), distinct from the inline-argument cases above. The
        // local is captured, never lowered as a value; the verb resolves the
        // name back to its slice. Same contract as kSlice.
        {"kNamed",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[64];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            rowF[lane] = rowIn[lane];\n"
         "            cfSh[lane * 2] = cfIn[lane];\n"
         "            cfSh[lane * 2 + 1] = -999.0f;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n"
         "        WaveVector<float32,16> cw = WaveVector.ofSlice(cfSh, 0, 2);\n"
         "        acc.scaledAccumInto(facc, rowF, cw);\n"
         "        facc.store(out, 0, 0, 16);\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        // scaledAccumI32 via ofSlice: an int accumulator, an int column panel.
        // iacc[r][c] += colS[c]*acc[r][c] = (1+c)*3, independent of r. This is
        // the case that was native-only until the software tile learned to read
        // the per-column int scales from Shared.
        {"kI32",
         "        Shared<int32> csSh = shared int32[64];\n"
         "        uint32 lane = KernelThread.x();\n"
         "        if (lane < 16) {\n"
         "            csSh[lane * 2] = (int32) (lane + 1);\n"
         "            csSh[lane * 2 + 1] = -999;\n"
         "        }\n"
         "        Barrier.workgroup();\n"
         "        CooperativeMatrix<int32,16,16,2> mc;\n"
         "        mc.splat(3);\n"
         "        CooperativeMatrix<int32,16,16,2> iacc;\n"
         "        iacc.splat(0);\n"
         "        mc.scaledAccumI32(iacc, WaveVector.ofSlice(csSh, 0, 2));\n"
         "        iacc.store(out, 0, 0, 16);\n",
         "3.0f * (1.0f + (float32) c)", true},

        // The killer shape: the ofSlice epilogue under a wave-divergent guard
        // with a panel-staging barrier. Only workgroup 0 does anything; the
        // launch runs two workgroups and the C++ side reads only tile 0.
        {"kGuard",
         "        Shared<float32> rowF = shared float32[16];\n"
         "        Shared<float32> cfSh = shared float32[32];\n"
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        uint32 wg = KernelThread.globalIdX() / 32;\n"
         "        if (wg < 1) {\n"
         "            uint32 lane = KernelThread.x();\n"
         "            if (lane < 16) {\n"
         "                rowF[lane] = rowIn[lane];\n"
         "                cfSh[lane * 2] = cfIn[lane];\n"
         "            }\n"
         "            Barrier.workgroup();\n"
         "            acc.splat(2.0f);\n"
         "            facc.splat(0.0f);\n"
         "            acc.scaledAccumInto(facc, rowF, WaveVector.ofSlice(cfSh, 0, 2));\n"
         "            facc.store(out, 0, 0, 16);\n"
         "        }\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},
    };
    return c;
}

std::string program() {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.Barrier;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "import cajeta.xpu.KernelThread;\n"
        "import cajeta.xpu.Shared;\n"
        "import cajeta.xpu.WaveVector;\n"
        "public final class D {\n";

    for (const Case& c : cases()) {
        s += "    @Kernel\n    public static void ";
        s += c.name;
        s += "(KernelBuffer<float32> rowIn,\n"
             "            KernelBuffer<float32> cfIn,\n"
             "            KernelBuffer<";
        s += c.intOut ? "int32" : "float32";
        s += "> out) {\n";
        s += c.body;
        s += "    }\n";
    }

    s += "    public static int32 run() {\n"
         "        float32[] hr = heap float32[16];\n"
         "        float32[] hc = heap float32[16];\n"
         "        int32 i = 0;\n"
         "        while (i < 16) {\n"
         "            hr[i] = 1.0f + (float32) i;\n"
         "            hc[i] = 1.0f + (float32) i;\n"
         "            i = i + 1;\n"
         "        }\n"
         "        KernelBuffer<float32> rowIn = heap KernelBuffer<float32>(16);\n"
         "        KernelBuffer<float32> cfIn = heap KernelBuffer<float32>(16);\n"
         "        rowIn.upload(hr);\n"
         "        cfIn.upload(hc);\n"
         "        KernelBuffer<float32> out = heap KernelBuffer<float32>(256);\n"
         "        float32[] ho = heap float32[256];\n"
         // An int32 accumulator stores int32 words; a separate buffer, as the
         // coop conformance harness learned to keep.
         "        KernelBuffer<int32> outi = heap KernelBuffer<int32>(256);\n"
         "        int32[] hoi = heap int32[256];\n"
         "        KernelStream s #= KernelStream.current();\n"
         "        int32 mask = 0;\n";
    int bit = 0;
    for (const Case& c : cases()) {
        const char* buf  = c.intOut ? "outi" : "out";
        const char* host = c.intOut ? "hoi"  : "ho";
        s += "        {\n            int32 z = 0;\n";
        s += std::string("            while (z < 256) { ") + host + "[z] = "
           + (c.intOut ? "-777" : "-777.0f") + "; z = z + 1; }\n";
        s += std::string("            ") + buf + ".upload(" + host + ");\n";
        s += std::string("            ") + c.name
           + ".launch(s, grid: [2], block: [32])(rowIn, cfIn, " + buf + ");\n"
             "            s.sync();\n";
        s += std::string("            ") + buf + ".download(" + host + ");\n"
             "            int32 r = 0;\n"
             "            int32 bad = 0;\n"
             "            while (r < 16) {\n"
             "                int32 c = 0;\n"
             "                while (c < 16) {\n";
        s += std::string("                    float32 want = ") + c.want + ";\n";
        s += std::string("                    if ((float32) ") + host
           + "[r * 16 + c] != want) { bad = bad + 1; }\n"
             "                    c = c + 1;\n"
             "                }\n"
             "                r = r + 1;\n"
             "            }\n";
        s += "            if (bad > 0) { mask = mask + " + std::to_string(1 << bit) + "; }\n";
        s += "        }\n";
        ++bit;
    }
    s += "        return mask;\n"
         "    }\n"
         "}\n";
    return s;
}

struct Outcome { int32_t mask; std::string err; };

Outcome runOn(cajeta::xpu::Backend be, bool launch) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(program(), "test.D", o);
    int32_t mask = 0;
    if (jit && launch) {
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (fn) mask = fn();
    }
    return {mask, testing::internal::GetCapturedStderr()};
}

std::string skipNoteFor(const std::string& err, const char* kernel) {
    const std::string tag = std::string("[xpu-kernel-skipped] ") + kernel;
    size_t at = err.find(tag);
    if (at == std::string::npos) return "";
    size_t nl = err.find('\n', at);
    return err.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
}
bool refused(const std::string& err, const char* kernel) {
    const std::string note = skipNoteFor(err, kernel);
    return !note.empty() && note.find("no assembler") == std::string::npos;
}
bool unassembled(const std::string& err, const char* kernel) {
    return skipNoteFor(err, kernel).find("no assembler") != std::string::npos;
}

struct BackendCase { const char* label; cajeta::xpu::Backend be; bool live; };
std::vector<BackendCase> backends() {
    return {
        {"cpu",   cajeta::xpu::Backend::Cpu,   true},
        {"nvptx", cajeta::xpu::Backend::Nvptx, cajeta::xpu::test::cudaAvailable()},
    };
}

}  // namespace

// ofSlice (with a real stride and base) and broadcast lower and are correct on
// every tier that is here, and NONE of them is refused: unlike ofLane, a
// slice-fed or broadcast column is expressible on the portable software tile.
TEST(XpuWaveVectorSlice, ofSliceAndBroadcastAreCorrectAndNeverRefusedOnAnyTier) {
    for (const BackendCase& b : backends()) {
        SCOPED_TRACE(b.label);
        Outcome o = runOn(b.be, b.live);
        int bit = 0;
        for (const Case& c : cases()) {
            const int32_t myBit = 1 << bit++;
            SCOPED_TRACE(c.name);
            if (unassembled(o.err, c.name)) continue;  // box has no assembler
            EXPECT_FALSE(refused(o.err, c.name))
                << c.name << " was refused on " << b.label
                << " — ofSlice/broadcast must lower on every tile:\n" << o.err;
            if (!b.live || refused(o.err, c.name)) continue;
            EXPECT_EQ(o.mask & myBit, 0)
                << c.name << " lowered on " << b.label
                << " and disagreed with its contract (a wrong/ignored stride "
                   "reads the poisoned slots).";
        }
    }
}
