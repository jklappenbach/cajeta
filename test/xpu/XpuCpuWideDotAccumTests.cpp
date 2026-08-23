// The CPU backend's `dotAccum` must reach the SAME tier an ordinary method
// does — because the CPU backend's ISA *is* the host's.
//
// It did not. `dotAccum` on a device routes through the per-lane
// `integerDot4x8` seam, which is the right shape on a GPU (each thread does
// scalar work) and the wrong one on x86: `vpdpbusd` consumes sixteen int8
// lanes at a time and the seam hands it four, so every kernel dotAccum fell
// to the portable widening reduce. Measured in ONE binary of the engine: the
// host form of a Q4_K/q8_K mat-vec got 8 `vpdpbusd`; the identical @Kernel
// got 8 `vpmaddwd`. End to end that was a 1.19x CPU decode where the fixed
// path gives 1.47x.
//
// Same family as the other silent-downgrade defects in this backend: it
// builds, it runs, the ANSWER IS RIGHT, and only the speed is wrong — which
// is why the tier is asserted here on the emitted IR and not inferred from
// a clock.

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

// Whether THIS machine has VNNI. The assertions below are about which
// instruction the host's own subtarget permits, so on a machine without it
// the right outcome is to skip, never to fail.
bool hostHasVnni() {
#if defined(__x86_64__) || defined(_M_X64)
    static const bool v = [] {
        std::string c = capture("\"" + compilerPath() + "\" --version --verbose");
        // The compiler prints its host triple/cpu; fall back to /proc.
        std::ifstream f("/proc/cpuinfo");
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        return s.find("avx512_vnni") != std::string::npos
            || s.find("avx_vnni") != std::string::npos;
    }();
    return v;
#else
    return false;
#endif
}

// Emit IR (the default mode) for `source` and return it.
std::string buildIr(const std::string& source, const char* entry,
                    const char* env = "") {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_widedot_" + std::to_string(rng()));
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "K.cajeta") << source;
    const std::string out = (base / "out").string();
    const std::string cmd = std::string(env)
        + "\"" + compilerPath() + "\""
        + " --release --emit=ir --xpu-backend=cpu"
        + " -o \"" + out + "\" " + entry
        + " \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    capture(cmd);
    std::string ir;
    for (const auto& e : fs::recursive_directory_iterator(base)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".ll" && e.path().extension() != ".bc")
            continue;
        if (e.path().extension() == ".bc") continue;
        std::ifstream f(e.path());
        ir += std::string((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    }
    if (ir.empty() && fs::exists(out)) {
        std::ifstream f(out);
        ir = std::string((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    fs::remove_all(base, ec);
    return ir;
}

// 32 unsigned nibbles against 32 signed activations -> Vector<int32,8>:
// exactly the Q4_K/q8_K shape, and exactly vpdpbusd's.
const char* WIDE = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class K {
    @Kernel
    public static void wk(KernelBuffer<int32> o, KernelBuffer<int8> w,
            KernelBuffer<int8> a, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) {
            Vector<int32,8> z = o.vload<8>(0) * 0;
            Vector<uint8,32> u = w.vload<32>(0).asUnsigned();
            Vector<uint8,32> m = u & 15;
            Vector<int32,8> r = m.dotAccum(a.vload<32>(0), z);
            o[(int64) g] = r[0];
        }
    }
    public static void run() { System.stdout.println("built"); }
}
)CJ";

// The SIGNED receiver — dotAccum's wide x86 tier is unsigned-weight only
// (vpdpbusd is u8 x i8), so this one must NOT take it.
const char* SIGNED_RECV = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class K {
    @Kernel
    public static void wk(KernelBuffer<int32> o, KernelBuffer<int8> w,
            KernelBuffer<int8> a, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) {
            Vector<int32,8> z = o.vload<8>(0) * 0;
            Vector<int8,32> s = w.vload<32>(0);
            Vector<int32,8> r = s.dotAccum(a.vload<32>(0), z);
            o[(int64) g] = r[0];
        }
    }
    public static void run() { System.stdout.println("built"); }
}
)CJ";

}  // namespace

// FIRES: an unsigned 32-lane dotAccum in a @Kernel reaches vpdpbusd, the same
// instruction the identical expression gets in an ordinary method.
TEST(XpuCpuWideDotAccumTests, kernelDotAccumReachesVnniLikeTheHostPath) {
    if (!hostHasVnni()) GTEST_SKIP() << "host has no VNNI";
    const std::string ir = buildIr(WIDE, "probe.K.run");
    ASSERT_FALSE(ir.empty()) << "no IR emitted";
    EXPECT_NE(ir.find("vpdpbusd"), std::string::npos)
        << "kernel dotAccum did not reach VNNI";
}

// DOES NOT FIRE, varying the MECHANISM rather than the expectation: a signed
// receiver is not vpdpbusd's shape (u8 x i8), so the same build must fall
// back to the per-lane seam. Without this, a change that emitted vpdpbusd
// unconditionally — silently reinterpreting signed weights as unsigned, a
// WRONG ANSWER — would pass the test above.
TEST(XpuCpuWideDotAccumTests, aSignedReceiverDoesNotTakeTheUnsignedWideTier) {
    if (!hostHasVnni()) GTEST_SKIP() << "host has no VNNI";
    const std::string ir = buildIr(SIGNED_RECV, "probe.K.run");
    ASSERT_FALSE(ir.empty()) << "no IR emitted";
    EXPECT_EQ(ir.find("vpdpbusd"), std::string::npos)
        << "a signed receiver took the unsigned-only VNNI tier";
}

// The scalar-fallback control has to STAY a control: with it set, no tier
// selection happens at all. A fast path that ignored the override would make
// every tier-agreement test in this suite vacuous.
TEST(XpuCpuWideDotAccumTests, scalarFallbackOverrideStillSuppressesTheWideTier) {
    if (!hostHasVnni()) GTEST_SKIP() << "host has no VNNI";
    const std::string ir =
        buildIr(WIDE, "probe.K.run", "CAJETA_SIMD_SCALAR_FALLBACK=1 ");
    ASSERT_FALSE(ir.empty()) << "no IR emitted";
    EXPECT_EQ(ir.find("vpdpbusd"), std::string::npos)
        << "CAJETA_SIMD_SCALAR_FALLBACK did not suppress the wide tier";
}
