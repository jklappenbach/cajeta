// Device.activeBackend() — asking the runtime WHICH backend it selected.
//
// Launch geometry is a device property, and until now nothing in the
// language could ask which device it got. The measurement that forced the
// question: a packed mat-vec split into 80-workgroup dispatches (one wave
// per SIMD) runs 1.7x faster end to end on gfx1151 and 6% SLOWER on the CPU
// backend, whose persistent worker pool pays per dispatch and has no
// occupancy cliff to avoid. Without this query a library has to pick one of
// those and be wrong on the other target.
//
// The contract has a sharp edge worth a test of its own: asking is a DEVICE
// TOUCH. It selects, so a later Device.force() throws. That is inherited
// from Device.supports(), and a doc comment claiming it is not evidence.
//
// AOT, for the reason XpuCpuDotAccumTierTests records.

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
#include "../PortableEnv.h"

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
    FILE* p = popen((cmd + " 2>" CAJETA_PORTABLE_DEVNULL "").c_str(), "r");
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

std::string buildAndRun(const std::string& source, const char* entry) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_abe_" + std::to_string(rng()));
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "B.cajeta") << source;
    const std::string exe = (base / "out").string();
    const std::string cmd = "\"" + compilerPath() + "\""
        + " --release --emit=exe --xpu-backend=cpu"
        + " -o \"" + exe + "\" " + entry
        + " \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    capture(cmd + " 2>&1");
    std::string out;
    if (fs::exists(exe)) out = capture("\"" + exe + "\"");
    std::error_code ec;
    fs::remove_all(base, ec);
    return out;
}

// A backend is bundled only when the program HAS a kernel — the compiler
// emits the registration ctor per bundled backend alongside the kernels it
// found. So every probe that expects a real selection declares one.
const char* REPORT = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class B {
    @Kernel
    public static void touch(KernelBuffer<int32> out, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) { out[g] = (int32) g; }
    }
    public static void run() {
        System.stdout.println("backend=" + Device.activeBackend());
    }
}
)CJ";

const char* FORCE_THEN_ASK = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class B {
    @Kernel
    public static void touch(KernelBuffer<int32> out, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) { out[g] = (int32) g; }
    }
    public static void run() {
        Device.force("cpu");
        System.stdout.println("backend=" + Device.activeBackend());
    }
}
)CJ";

const char* ASK_THEN_FORCE = R"CJ(
package probe;
import cajeta.error.RecoverableException;
import cajeta.lang.System;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class B {
    @Kernel
    public static void touch(KernelBuffer<int32> out, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) { out[g] = (int32) g; }
    }
    public static void run() {
        System.stdout.println("backend=" + Device.activeBackend());
        try {
            Device.force("cpu");
            System.stdout.println("force=accepted");
        } catch (RecoverableException e) {
            System.stdout.println("force=threw");
        }
    }
}
)CJ";

const char* NO_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
public class B {
    public static void run() {
        System.stdout.println("backend=" + Device.activeBackend());
    }
}
)CJ";

}  // namespace

// Built with --xpu-backend=cpu and nothing else, so the ONLY backend that can
// be selected is the CPU one. Anything else means the query is not reading
// the runtime's real selection.
TEST(XpuActiveBackendTests, reportsTheBackendTheRuntimeSelected) {
    const std::string out = buildAndRun(REPORT, "probe.B.run");
    EXPECT_NE(out.find("backend=cpu"), std::string::npos) << out;
}

// The does-NOT-fire half, and it varies the MECHANISM rather than the
// expected string: the same --xpu-backend=cpu build of a program with NO
// kernel bundles no backend at all, because the registration ctor is emitted
// per bundled backend alongside the kernels the compiler found. So the query
// must answer "none" here and "cpu" above, off the same compiler flag. A
// stub returning a constant cannot produce both, and — measured — a probe
// asserting only "not none" would have passed against a build where the
// answer was wrong for this exact reason.
TEST(XpuActiveBackendTests, reportsNoneWhenTheProgramBundlesNoBackend) {
    const std::string out = buildAndRun(NO_KERNEL, "probe.B.run");
    EXPECT_NE(out.find("backend=none"), std::string::npos) << out;
    EXPECT_EQ(out.find("backend=cpu"), std::string::npos) << out;
}

// force() then ask: the vocabularies are the same, so what force() accepts is
// what activeBackend() hands back.
TEST(XpuActiveBackendTests, agreesWithAnExplicitForce) {
    const std::string out = buildAndRun(FORCE_THEN_ASK, "probe.B.run");
    EXPECT_NE(out.find("backend=cpu"), std::string::npos) << out;
}

// Ask then force: asking SELECTS, so the force must be rejected. This is the
// documented edge, and without it the doc comment is a claim rather than a
// contract.
TEST(XpuActiveBackendTests, askingIsADeviceTouchSoALaterForceThrows) {
    const std::string out = buildAndRun(ASK_THEN_FORCE, "probe.B.run");
    EXPECT_NE(out.find("backend=cpu"), std::string::npos) << out;
    EXPECT_NE(out.find("force=threw"), std::string::npos) << out;
    EXPECT_EQ(out.find("force=accepted"), std::string::npos) << out;
}
