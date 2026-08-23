// Device-side correctness for `dotAccum` — the test whose absence let a
// silent wrong answer ship on every device backend.
//
// `dotAccum` means unsigned weights x SIGNED activations. That asymmetry is
// the reason the instruction family exists (vpdpbusd, usdot, vqdotsu), and
// llama.cpp reaches the same shape from the other direction: it normalizes
// signed x signed INTO unsigned x signed with a sign-transfer
// (`_mm256_sign_epi8(x,x)` / `_mm256_sign_epi8(y,x)`) because u x s is the
// only form the hardware offers.
//
// The device lowering seam used to carry ONE signedness flag for both
// operands, so with an unsigned receiver it zero-extended the activations
// too: measured 6440 against the host's -1240, on the CPU backend AND on a
// real gfx1151. The seam now takes two flags; AMDGPU reaches
// `amdgcn.sudot4` (per-operand sign bits) for the mixed case and Vulkan
// falls back to the portable widen, since stock LLVM exposes no mixed
// SPIR-V dot intrinsic.
//
// These are AOT tests on purpose. They cannot be written against CajetaJit:
// it wires no XPU backend manifest, so a kernel launch there silently
// no-ops ("cajeta.xpu: no available backend among {}") and every buffer
// reads back zero — a test written that way passes while verifying nothing.
// That is not hypothetical: XpuScheduleTests.scheduleControlsRunNoOpOnCpu
// compares two all-zero outputs and would pass with no kernel running at
// all.

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string compilerPath() {
    std::string r;
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    if (r.size() >= 3 && r[0] == '/' && std::isalpha((unsigned char) r[1])
            && r[2] == '/')
        r = std::string(1, r[1]) + ":" + r.substr(2);
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

std::string capture(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen((cmd + " 2>NUL").c_str(), "r");
#else
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
#endif
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
#ifdef _WIN32
    _pclose(p);
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
#else
    pclose(p);
#endif
    return out;
}

// Build `source` as an exe for `backend` and run it, returning its stdout.
// Empty means the build failed — asserted by the callers, since a silent
// empty result is exactly the failure mode this file exists to prevent.
std::string buildAndRun(const std::string& source, const char* backend,
                        const char* entry) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_dotacc_" + std::to_string(rng()));
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "K.cajeta") << source;
    const std::string exe = (base / "out").string();
    const std::string cmd = "\"" + compilerPath() + "\""
        + " --release --emit=exe --xpu-backend=" + backend
        + " -o \"" + exe + "\" " + entry
        + " \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    capture(cmd);
    if (!fs::exists(exe)) return {};
    std::string out = capture("\"" + exe + "\"");
    std::error_code ec;
    fs::remove_all(base, ec);
    return out;
}

// Weights are unsigned nibbles in the Q4_K range; activations span the
// signed int8 range and are genuinely negative, which is the whole point —
// with the operands treated symmetrically the answer changes sign.
const char* DOTACC_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class K {
    @Kernel
    public static void accK(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                            KernelBuffer<int8> a, KernelBuffer<int32> seed,
                            uint32 groups) {
        uint32 g = KernelThread.globalIdX();
        if (g < groups) {
            int64 wo = (int64) g * 64L;
            int64 so = (int64) g * 16L;
            Vector<uint8,64> wv = w.vload<64>(wo);
            Vector<int8,64> av = a.vload<64>(wo);
            Vector<int32,16> acc = seed.vload<16>(so);
            out.vstore(so, wv.dotAccum(av, acc));
        }
    }
    public static void run() {
        uint32 groups = 8;
        uint32 n = groups * 64;
        uint32 accN = groups * 16;
        uint8[] hw #= heap uint8[n];
        int8[] ha #= heap int8[n];
        int32[] hs #= heap int32[accN];
        int32[] ho #= heap int32[accN];
        uint32 i = 0;
        while (i < n) {
            hw[i] = (uint8) ((i * 5) % 16);
            ha[i] = (int8) (((i * 37) % 255) - 127);
            i = i + 1;
        }
        i = 0;
        while (i < accN) { hs[i] = (int32) i * 3 - 20; i = i + 1; }
        KernelBuffer<int32> out = heap KernelBuffer<int32>(accN);
        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(n);
        KernelBuffer<int8> a = heap KernelBuffer<int8>(n);
        KernelBuffer<int32> seed = heap KernelBuffer<int32>(accN);
        w.upload(hw); a.upload(ha); seed.upload(hs);
        KernelStream s #= KernelStream.current();
        accK.launch(s, grid: [groups], block: [1])(out, w, a, seed, groups);
        s.sync();
        out.download(ho);
        int32 bad = -1;
        uint32 lane = 0;
        while (lane < accN) {
            int32 want = hs[lane];
            uint32 k = 0;
            while (k < 4) {
                want = want + (int32) hw[lane * 4 + k] * (int32) ha[lane * 4 + k];
                k = k + 1;
            }
            if (ho[lane] != want && bad < 0) { bad = (int32) lane; }
            lane = lane + 1;
        }
        if (bad < 0) { System.stdout.println("RESULT ok"); }
        else { System.stdout.println("RESULT bad lane " + bad
            + " got " + ho[bad]); }
        return;
    }
}
)CJ";

// `dot` is SYMMETRIC and must stay so. This is the "does not fire" half:
// a fix that leaked into `dot` would turn 6460 into -1220 and erase the
// difference the two spellings exist to express.
const char* DOT_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class K {
    @Kernel
    public static void symK(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                            KernelBuffer<int8> a) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            // Named locals, deliberately. A CHAINED receiver
            // (`w.vload<4>(0L).dot(...)`) silently produces zero on the
            // kernel path: the lowering resolves the receiver by VARIABLE
            // NAME (`values[recv]`), so a temporary has no entry. Filed
            // separately; this test is about signedness, not that.
            Vector<uint8,4> wv = w.vload<4>(0L);
            Vector<int8,4> av = a.vload<4>(0L);
            out[0] = wv.dot(av);
        }
    }
    public static void run() {
        uint8[] hw #= heap uint8[4];
        int8[] ha #= heap int8[4];
        uint32 i = 0;
        while (i < 4) {
            hw[i] = (uint8) ((i * 5) % 16);
            ha[i] = (int8) (((i * 37) % 255) - 127);
            i = i + 1;
        }
        int32[] ho #= heap int32[1];
        KernelBuffer<int32> out = heap KernelBuffer<int32>(1);
        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(4);
        KernelBuffer<int8> a = heap KernelBuffer<int8>(4);
        w.upload(hw); a.upload(ha);
        KernelStream s #= KernelStream.current();
        symK.launch(s, grid: [1], block: [1])(out, w, a);
        s.sync();
        out.download(ho);
        int32 sym = 0;
        i = 0;
        while (i < 4) {
            sym = sym + (int32) hw[i] * ((int32) ha[i] & 255);
            i = i + 1;
        }
        if (ho[0] == sym) { System.stdout.println("RESULT ok"); }
        else { System.stdout.println("RESULT bad got " + ho[0]
            + " want " + sym); }
        return;
    }
}
)CJ";

}  // namespace

TEST(XpuCpuDotAccumTests, dotAccumInKernelIsUnsignedTimesSigned) {
    const std::string out = buildAndRun(DOTACC_KERNEL, "cpu", "probe.K.run");
    ASSERT_FALSE(out.empty()) << "probe failed to build or produced no output";
    EXPECT_NE(out.find("RESULT ok"), std::string::npos)
        << "dotAccum in a CPU-backend kernel body disagrees with the scalar "
           "reference; a symmetric signedness flag reads the signed "
           "activations as unsigned. Got: " << out;
}

// The control. Same seam, same backend, the spelling that MUST be symmetric.
TEST(XpuCpuDotAccumTests, dotInKernelStaysSymmetric) {
    const std::string out = buildAndRun(DOT_KERNEL, "cpu", "probe.K.run");
    ASSERT_FALSE(out.empty()) << "probe failed to build or produced no output";
    EXPECT_NE(out.find("RESULT ok"), std::string::npos)
        << "`dot` must stay symmetric on an unsigned receiver — only "
           "dotAccum is unsigned x signed. Got: " << out;
}
