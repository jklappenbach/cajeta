// The integer LADDER inside a kernel — widenLo / widenHi / narrow / toF32 /
// toI32.
//
// The packed mat-vecs are built on it: `q4kMatVecInto` reads 64 bytes, masks
// nibbles, walks int8 -> int16 -> int32 -> f32 and accumulates. Every rung
// was host-only, so the whole body was unliftable into a @Kernel — and it
// failed the way this suite exists to catch. A probe doing
//
//     raw.vload<64>(0) & 15  ->  widenLo()  ->  widenLo()  ->  toF32()
//
// COMPILED CLEAN and printed zeros: the CPU backend caught the lowering
// exception, skipped the kernel, and the launch found nothing to run. The
// diagnostic existed but was gated behind CAJETA_XPU_DEBUG_LOWER, so the
// default build said nothing. (The amdgpu, nvptx and vulkan backends all
// print `[xpu-kernel-skipped]` unconditionally; the CPU backend — the
// DEFAULT one — was the only holdout.)
//
// The lowerings themselves are target-neutral and already existed in
// `vecops`; what was missing was the kernel-side dispatch to them.
//
// AOT on purpose, for the reason XpuCpuDotAccumTierTests records: a kernel
// launched under a JIT with no backend manifest silently no-ops and every
// buffer reads back zero, so a test written that way passes while verifying
// nothing.

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

// `mergeErr` decides whether the build's own diagnostics come back. The
// ladder tests want only the program's stdout; the skip-diagnostic test
// wants what the COMPILER said.
std::string capture(const std::string& cmd, bool mergeErr = false) {
    std::string out;
    const std::string full = cmd + (mergeErr ? " 2>&1"
#ifdef _WIN32
                                             : " 2>NUL");
#else
                                             : " 2>/dev/null");
#endif
#ifdef _WIN32
    FILE* p = _popen(full.c_str(), "r");
#else
    FILE* p = popen(full.c_str(), "r");
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

struct Built {
    std::string buildLog;   // what the compiler said
    std::string runOut;     // what the program printed, empty if it never built
};

Built buildAndRun(const std::string& source, const char* backend,
                  const char* entry) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_ladder_" + std::to_string(rng()));
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "L.cajeta") << source;
    const std::string exe = (base / "out").string();
    const std::string cmd = "\"" + compilerPath() + "\""
        + " --release --emit=exe --xpu-backend=" + backend
        + " -o \"" + exe + "\" " + entry
        + " \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    Built b;
    b.buildLog = capture(cmd, /*mergeErr=*/true);
    if (fs::exists(exe)) b.runOut = capture("\"" + exe + "\"");
    std::error_code ec;
    fs::remove_all(base, ec);
    return b;
}

// The Q4_K shape exactly: 64 packed bytes, low nibbles, three rungs up to
// f32, then the `q*term - mterm` fold `q4kAcc` performs. `term` and `mterm`
// are powers of two and `q` is 0..15, so every result is exactly
// representable and the comparison is hard equality, not a tolerance.
const char* LADDER_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void ladderK(KernelBuffer<float32> out,
                               KernelBuffer<int8> raw) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            Vector<int8,64> r = raw.vload<64>(0L);
            Vector<int8,64> lo = r & 15;
            Vector<int16,32> w = lo.widenLo();
            Vector<int32,16> wl = w.widenLo();
            Vector<int32,16> wh = w.widenHi();
            out.vstore(0L, wl.toF32());
            out.vstore(16L, wh.toF32() * 0.5f - 2.0f);
        }
    }
    public static void run() {
        int8[] hr #= heap int8[64];
        float32[] ho #= heap float32[32];
        uint32 i = 0;
        // Genuinely negative bytes: `& 15` must survive the trip up the
        // ladder, and a sign-extend of the MASKED value is a no-op only
        // because the mask ran first.
        while (i < 64) { hr[i] = (int8) ((int32) (i * 7) - 100); i = i + 1; }
        KernelBuffer<float32> out = heap KernelBuffer<float32>(32);
        KernelBuffer<int8> raw = heap KernelBuffer<int8>(64);
        raw.upload(hr);
        KernelStream s #= KernelStream.current();
        ladderK.launch(s, grid: [1], block: [1])(out, raw);
        s.sync();
        out.download(ho);
        int32 bad = -1;
        i = 0;
        while (i < 16) {
            float32 q0 = (float32) (((int32) hr[i]) & 15);
            float32 q1 = (float32) (((int32) hr[i + 16]) & 15);
            if (ho[i] != q0 && bad < 0) { bad = (int32) i; }
            if (ho[i + 16] != q1 * 0.5f - 2.0f && bad < 0) {
                bad = (int32) i + 100;
            }
            i = i + 1;
        }
        if (bad < 0) { System.stdout.println("RESULT ok"); }
        else { System.stdout.println("RESULT bad at " + bad
            + " got " + ho[bad % 100] ); }
        return;
    }
}
)CJ";

// Unsigned elements ZERO-extend. 0xF0 is 240, not -16 — and Q4_K's nibbles
// reach `dotAccum` through exactly this rung.
const char* UNSIGNED_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void uwK(KernelBuffer<int32> out, KernelBuffer<uint8> raw) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            Vector<uint8,16> r = raw.vload<16>(0L);
            Vector<uint16,8> w = r.widenLo();
            Vector<uint32,4> ww = w.widenLo();
            Vector<int32,4> sv = ww.asSigned();
            out.vstore(0L, sv);
        }
    }
    public static void run() {
        uint8[] hr #= heap uint8[16];
        int32[] ho #= heap int32[4];
        uint32 i = 0;
        while (i < 16) { hr[i] = (uint8) 240; i = i + 1; }
        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);
        KernelBuffer<uint8> raw = heap KernelBuffer<uint8>(16);
        raw.upload(hr);
        KernelStream s #= KernelStream.current();
        uwK.launch(s, grid: [1], block: [1])(out, raw);
        s.sync();
        out.download(ho);
        System.stdout.println("RESULT " + ho[0]);
        return;
    }
}
)CJ";

// The does-NOT-fire control. Same rung, signed elements: (int8) 0xF0 is -16
// and must stay -16. A "fix" that zero-extended everything reads 240 here,
// and the unsigned test above would not notice.
const char* SIGNED_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void swK(KernelBuffer<int32> out, KernelBuffer<int8> raw) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            Vector<int8,16> r = raw.vload<16>(0L);
            Vector<int16,8> w = r.widenLo();
            Vector<int32,4> ww = w.widenLo();
            out.vstore(0L, ww);
        }
    }
    public static void run() {
        int8[] hr #= heap int8[16];
        int32[] ho #= heap int32[4];
        uint32 i = 0;
        while (i < 16) { hr[i] = (int8) -16; i = i + 1; }
        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);
        KernelBuffer<int8> raw = heap KernelBuffer<int8>(16);
        raw.upload(hr);
        KernelStream s #= KernelStream.current();
        swK.launch(s, grid: [1], block: [1])(out, raw);
        s.sync();
        out.download(ho);
        System.stdout.println("RESULT " + ho[0]);
        return;
    }
}
)CJ";

// `narrow` is the ladder's inverse and `toI32` its float exit. Both are
// wired here because half a ladder is how the widen rungs went missing in
// the first place.
const char* NARROW_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void nwK(KernelBuffer<int32> out, KernelBuffer<int8> raw) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            Vector<int8,16> r = raw.vload<16>(0L);
            Vector<int16,8> lo = r.widenLo();
            Vector<int16,8> hi = r.widenHi();
            Vector<int8,16> back = lo.narrow(hi);
            Vector<int16,8> rt = back.widenLo();
            Vector<int32,4> w32 = rt.widenLo();
            Vector<float32,4> f = w32.toF32() * 2.0f;
            Vector<int32,4> back32 = f.toI32();
            out.vstore(0L, back32);
        }
    }
    public static void run() {
        int8[] hr #= heap int8[16];
        int32[] ho #= heap int32[4];
        uint32 i = 0;
        while (i < 16) { hr[i] = (int8) ((int32) i - 5); i = i + 1; }
        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);
        KernelBuffer<int8> raw = heap KernelBuffer<int8>(16);
        raw.upload(hr);
        KernelStream s #= KernelStream.current();
        nwK.launch(s, grid: [1], block: [1])(out, raw);
        s.sync();
        out.download(ho);
        int32 bad = -1;
        i = 0;
        while (i < 4) {
            int32 want = (((int32) i) - 5) * 2;
            if (ho[i] != want && bad < 0) { bad = (int32) i; }
            i = i + 1;
        }
        if (bad < 0) { System.stdout.println("RESULT ok"); }
        else { System.stdout.println("RESULT bad at " + bad
            + " got " + ho[bad]); }
        return;
    }
}
)CJ";

// f16 <-> f32 lane conversion INSIDE a kernel. This is the rung that was
// missing when an f16 mat-vec probe failed to lower (plan 8.14): the
// ladder had an integer `toF32` and no float one.
const char* HALF_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void halfK(KernelBuffer<float32> out,
                             KernelBuffer<float16> h) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            Vector<float16,4> hv = h.vload<4>(0L);
            Vector<float32,4> f = hv.toF32() * 2.0f;
            out.vstore(0L, f);
            // ...and back down, so the rung is exercised both ways.
            Vector<float16,4> back = f.toF16();
            out.vstore(4L, back.toF32());
        }
    }
    public static void run() {
        float16[] hh #= heap float16[4];
        hh[0] = (float16) 1.5f;
        hh[1] = (float16) -2.25f;
        hh[2] = (float16) 0.5f;
        hh[3] = (float16) 10.0f;
        float32[] ho #= heap float32[8];
        KernelBuffer<float32> out = heap KernelBuffer<float32>(8);
        KernelBuffer<float16> h = heap KernelBuffer<float16>(4);
        h.upload(hh);
        KernelStream s #= KernelStream.current();
        halfK.launch(s, grid: [1], block: [1])(out, h);
        s.sync();
        out.download(ho);
        // 2*(1.5 - 2.25 + 0.5 + 10.0) = 19.5, twice (the round trip is
        // exact for these values).
        float32 a = ho[0] + ho[1] + ho[2] + ho[3];
        float32 b = ho[4] + ho[5] + ho[6] + ho[7];
        if (a == 19.5f && b == 19.5f) { System.stdout.println("RESULT ok"); }
        else { System.stdout.println("RESULT bad a=" + a + " b=" + b); }
        return;
    }
}
)CJ";

// A construct that cannot lower to a device: a heap allocation in a kernel
// body. The point is not that it is rejected — it is that the BUILD SAYS SO.
const char* UNLOWERABLE_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class L {
    @Kernel
    public static void cannotLowerK(KernelBuffer<int32> out) {
        uint32 g = KernelThread.globalIdX();
        if (g < 1) {
            int32[] scratch #= heap int32[4];
            scratch[0] = 7;
            out[0] = scratch[0];
        }
    }
    public static void run() {
        int32[] ho #= heap int32[4];
        KernelBuffer<int32> out = heap KernelBuffer<int32>(4);
        KernelStream s #= KernelStream.current();
        cannotLowerK.launch(s, grid: [1], block: [1])(out);
        s.sync();
        out.download(ho);
        System.stdout.println("RESULT " + ho[0]);
        return;
    }
}
)CJ";

}  // namespace

TEST(XpuCpuVectorLadderTests, widenAndConvertLadderLowersInAKernel) {
    const Built b = buildAndRun(LADDER_KERNEL, "cpu", "probe.L.run");
    ASSERT_FALSE(b.runOut.empty())
        << "probe failed to build or produced no output. Build said:\n"
        << b.buildLog;
    EXPECT_NE(b.runOut.find("RESULT ok"), std::string::npos)
        << "the int8 -> int16 -> int32 -> f32 ladder disagrees with the "
           "scalar reference inside a kernel. Got: " << b.runOut
        << "\nBuild said:\n" << b.buildLog;
}

TEST(XpuCpuVectorLadderTests, unsignedWidenZeroExtendsInAKernel) {
    const Built b = buildAndRun(UNSIGNED_KERNEL, "cpu", "probe.L.run");
    ASSERT_FALSE(b.runOut.empty())
        << "probe failed to build. Build said:\n" << b.buildLog;
    EXPECT_NE(b.runOut.find("RESULT 240"), std::string::npos)
        << "widenLo on an unsigned receiver must ZERO-extend: 0xF0 is 240, "
           "and -16 means it sign-extended. Got: " << b.runOut;
}

TEST(XpuCpuVectorLadderTests, signedWidenSignExtendsInAKernel) {
    const Built b = buildAndRun(SIGNED_KERNEL, "cpu", "probe.L.run");
    ASSERT_FALSE(b.runOut.empty())
        << "probe failed to build. Build said:\n" << b.buildLog;
    EXPECT_NE(b.runOut.find("RESULT -16"), std::string::npos)
        << "widenLo on a signed receiver must SIGN-extend: (int8) 0xF0 is "
           "-16, and 240 means the unsigned rule leaked across. Got: "
        << b.runOut;
}

TEST(XpuCpuVectorLadderTests, halfToF32AndBackLowerInAKernel) {
    const Built b = buildAndRun(HALF_KERNEL, "cpu", "probe.L.run");
    ASSERT_FALSE(b.runOut.empty())
        << "probe failed to build. Build said:\n" << b.buildLog;
    EXPECT_NE(b.runOut.find("RESULT ok"), std::string::npos)
        << "f16 <-> f32 lane conversion is wrong inside a kernel. Got: "
        << b.runOut;
}

TEST(XpuCpuVectorLadderTests, narrowAndToI32LowerInAKernel) {
    const Built b = buildAndRun(NARROW_KERNEL, "cpu", "probe.L.run");
    ASSERT_FALSE(b.runOut.empty())
        << "probe failed to build. Build said:\n" << b.buildLog;
    EXPECT_NE(b.runOut.find("RESULT ok"), std::string::npos)
        << "narrow / toI32 disagree with the scalar reference inside a "
           "kernel. Got: " << b.runOut;
}

// The diagnostic, not the rejection. A kernel the CPU backend cannot lower
// must be named at BUILD time — otherwise the launch no-ops and every
// output buffer reads zero, which is how the missing ladder hid.
TEST(XpuCpuVectorLadderTests, unlowerableKernelIsNamedAtBuildTime) {
    const Built b = buildAndRun(UNLOWERABLE_KERNEL, "cpu", "probe.L.run");
    EXPECT_NE(b.buildLog.find("[xpu-kernel-skipped]"), std::string::npos)
        << "the CPU backend skipped a kernel silently. amdgpu, nvptx and "
           "vulkan all print [xpu-kernel-skipped] unconditionally; the "
           "DEFAULT backend must too. Build said:\n" << b.buildLog;
    EXPECT_NE(b.buildLog.find("cannotLowerK"), std::string::npos)
        << "the skip diagnostic must name the kernel. Build said:\n"
        << b.buildLog;
}
