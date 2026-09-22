//
// CONFORMANCE, THE BREADTH HALF: every backend either honours every
// CooperativeMatrix verb's contract or refuses it BY NAME. Never a third
// thing.
//
// NvptxCoopConformanceTests checks the contract cell by cell on NVIDIA and
// says which column a wrong cell took. That is the depth half, and it is
// NVIDIA-only by construction — it drives the CUDA driver directly. This
// file is the other axis: the same seven argument shapes, compiled and run
// through the JIT on every backend, asserting the disjunction above.
//
// WHY THE DISJUNCTION IS THE PROPERTY. Three outcomes are possible when a
// backend meets a verb it cannot express, and only one of them is safe:
//
//   lowers and is right        fine
//   refuses, by name           fine — the author gets told what to write
//   lowers and is wrong        the failure this whole unit is about
//
// The third is what shipped: NVIDIA lowered `scaledAccumIntoS` by applying
// one lane's scalar to a fragment spanning four columns, and nothing
// anywhere asserted otherwise. AmdgpuCoopEpilogueTests drove the same verbs
// on AMD and its kernels end `run() { return 1; }` — it checks the LOWERING
// NOTES and never the numbers, so it could not have caught it either.
//
// ONE PROGRAM, SEVEN KERNELS, ONE COMPILE PER BACKEND. A separate JIT
// compile per (verb x backend) would be 28 of them at ~45s each. The
// program's `run()` returns a BITMASK of which verbs disagreed with their
// contract, so one failing verb is still localized to itself.
//
// rowF[r] = 1+r and colF[c] = 1+c are both ramps and every product stays
// under 2^24, so the comparison is exact and a transposed, rotated or
// broadcast mapping lands on a different number in nearly every cell.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "XpuRefusalProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;
using cajeta_test::catchRefusal;

namespace {

// verb -> the kernel that exercises it, and the bit it owns in run()'s mask.
struct Verb {
    const char* name;      // as declared in CooperativeMatrix.cajeta
    const char* kernel;    // @Kernel name, also what a skip note prints
    const char* decls;     // tile declarations
    const char* call;      // the verb call plus the store
    const char* want;      // the contract, as a cajeta expression in r and c
    bool intOut;
};

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> v = {
        {"rank1Accum", "kv0",
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        facc.rank1Accum(rowF, colF);\n"
         "        facc.store(out, 0, 0, 16);\n",
         "(1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"rank1Accum", "kv1",
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        facc.rank1Accum(rowF, WaveVector.ofLane(cv));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "(1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"scaledAccumInto", "kv2",
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        acc.scaledAccumInto(facc, rowF, colF);\n"
         "        facc.store(out, 0, 0, 16);\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"scaledAccumInto", "kv3",
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        acc.scaledAccumInto(facc, rowF, WaveVector.ofLane(cv));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "2.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"scaledAccumInto2", "kv4",
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        acc.scaledAccumInto2(facc, rowF, colF, rowF, colF);\n"
         "        facc.store(out, 0, 0, 16);\n",
         "3.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"scaledAccumInto2", "kv5",
         "        CooperativeMatrix<float32,16,16,2> acc;\n"
         "        acc.splat(2.0f);\n"
         "        CooperativeMatrix<float32,16,16,2> facc;\n"
         "        facc.splat(0.0f);\n",
         "        acc.scaledAccumInto2(facc, rowF, WaveVector.ofLane(cv), rowF, WaveVector.ofLane(cv));\n"
         "        facc.store(out, 0, 0, 16);\n",
         "3.0f * (1.0f + (float32) r) * (1.0f + (float32) c)", false},

        {"scaledAccumI32", "kv6",
         "        int32 cs = (int32) cv;\n"
         "        CooperativeMatrix<int32,16,16,2> mc;\n"
         "        mc.splat(3);\n"
         "        CooperativeMatrix<int32,16,16,2> iacc;\n"
         "        iacc.splat(0);\n",
         "        mc.scaledAccumI32(iacc, WaveVector.ofLane(cs));\n"
         "        iacc.store(out, 0, 0, 16);\n",
         "3.0f * (1.0f + (float32) c)", true},
    };
    return v;
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
        "import cajeta.xpu.XpuLaunchException;\n"
        "public final class D {\n";

    for (const Verb& v : verbs()) {
        s += "    @Kernel\n    public static void ";
        s += v.kernel;
        s += "(KernelBuffer<float32> rowIn,\n"
             "            KernelBuffer<float32> colIn,\n"
             "            KernelBuffer<";
        s += v.intOut ? "int32" : "float32";
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
        s += v.decls;
        s += v.call;
        s += "    }\n";
    }

    // One launch per verb; bit i is set when verb i disagreed with its
    // contract. A verb the backend REFUSED never registers, so its launch is
    // a no-op and the sentinel survives — the C++ side reads the skip note
    // to tell refusal from wrongness, which is the whole point.
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
         "        KernelBuffer<float32> colIn = heap KernelBuffer<float32>(16);\n"
         "        rowIn.upload(hr);\n"
         "        colIn.upload(hc);\n"
         "        KernelBuffer<float32> out = heap KernelBuffer<float32>(256);\n"
         "        float32[] ho = heap float32[256];\n"
         // An int32 accumulator stores int32 words. Giving it the float
         // buffer compiled and then disagreed with its own contract on
         // nvptx, which read exactly like a product bug for a minute — the
         // reason every case below names its own buffer.
         "        KernelBuffer<int32> outi = heap KernelBuffer<int32>(256);\n"
         "        int32[] hoi = heap int32[256];\n"
         "        KernelStream s #= KernelStream.current();\n"
         "        int32 mask = 0;\n";
    int bit = 0;
    for (const Verb& v : verbs()) {
        const char* buf  = v.intOut ? "outi" : "out";
        const char* host = v.intOut ? "hoi"  : "ho";
        s += "        {\n            int32 z = 0;\n";
        s += std::string("            while (z < 256) { ") + host + "[z] = "
           + (v.intOut ? "-777" : "-777.0f") + "; z = z + 1; }\n";
        s += std::string("            ") + buf + ".upload(" + host + ");\n";
        // A backend that REFUSES this verb registers no device code, so the
        // launch now RAISES XpuLaunchException (naming kernel + backend) rather
        // than silently no-opping. That is the refusal signal; catchRefusal
        // swallows it so the sentinel survives and the C++ side still reads the
        // compile-time skip note to tell refusal from wrongness -- same contract.
        s += catchRefusal(
            std::string("            ") + v.kernel
            + ".launch(s, grid: [1], block: [32])(rowIn, colIn, " + buf + ");\n"
              "            s.sync();\n");
        s += std::string("            ") + buf + ".download(" + host + ");\n"
             "            int32 r = 0;\n"
             "            int32 bad = 0;\n"
             "            while (r < 16) {\n"
             "                int32 c = 0;\n"
             "                while (c < 16) {\n";
        s += std::string("                    float32 want = ") + v.want + ";\n";
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

// Each backend, and whether its device is here to launch on.
struct BackendCase { const char* label; cajeta::xpu::Backend be; bool live; };

std::vector<BackendCase> backends() {
    return {
        {"cpu",    cajeta::xpu::Backend::Cpu,    true},
        {"nvptx",  cajeta::xpu::Backend::Nvptx,
                   cajeta::xpu::test::cudaAvailable()},
        {"amdgpu", cajeta::xpu::Backend::Amdgpu,
                   cajeta::xpu::test::hipAvailable()},
        {"spirv",  cajeta::xpu::Backend::Spirv,
                   cajeta::xpu::test::vulkanAvailable()},
    };
}

// The skip note for THIS kernel, or empty. Two different things print it:
// the lowering refusing a verb by contract, and — since 2026-09-21 — the
// registration failing to ASSEMBLE what did lower (no ld.lld, no ptxas). The
// second says "no assembler" and means "this box cannot build it", which is
// neither of the two outcomes this test judges; before that note existed the
// kernel simply vanished and the leg read as "lowered, no device", i.e. the
// silent absence.
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

} // namespace

// The disjunction, on every backend: honour the contract or refuse by name.
TEST(XpuCoopConformance, everyBackendHonoursOrRefusesEveryVerb) {
    for (const BackendCase& b : backends()) {
        SCOPED_TRACE(b.label);
        Outcome o = runOn(b.be, b.live);

        int bit = 0;
        for (const Verb& v : verbs()) {
            const int32_t myBit = 1 << bit++;
            SCOPED_TRACE(std::string("CooperativeMatrix.") + v.name);

            if (unassembled(o.err, v.kernel)) {
                // Lowered, but this box has no assembler for the backend.
                // Not a verdict on the verb — and now VISIBLE, where it used
                // to look exactly like a backend with no device attached.
                continue;
            }
            if (refused(o.err, v.kernel)) {
                // A refusal must say what the caller failed to provide and
                // what to write instead — B's contract wording. A bare
                // "unsupported" leaves the kernel silently absent.
                EXPECT_TRUE(o.err.find("lane L supplies") != std::string::npos
                            || o.err.find("Shared") != std::string::npos)
                    << "the refusal must state the contract and name the "
                       "working spelling:\n" << o.err;
                continue;
            }
            if (!b.live) continue;   // lowered; no device here to run it on
            EXPECT_EQ(o.mask & myBit, 0)
                << v.name << " lowered on " << b.label
                << " and disagreed with its contract — the outcome this test "
                   "exists to forbid. NvptxCoopConformanceTests prints which "
                   "column a wrong cell took.";
        }
    }
}
