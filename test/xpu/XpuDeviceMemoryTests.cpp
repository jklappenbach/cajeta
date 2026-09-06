// Device.memoryBytes() — the active device's total visible memory.
//
// The number a residency budget derives from (cajeta-llm's MoE expert
// cache is the motivating consumer: its spec requires a default "derived
// from device-visible memory", and until this query existed nothing in
// the language could ask — every geometry number in cajeta-llama is a
// literal for the same reason).
//
// The oracle is REAL, not a threshold: on the CPU backend the device IS
// the host, so the runtime's answer must EQUAL total physical RAM read
// through the same OS API this test calls directly. A plausibility bound
// (`> 0`) would pass a stub; exact agreement with an independently read
// value cannot. The no-backend arm pins the other half of the contract:
// 0 means UNKNOWN, and a program that bundles no backend must answer 0 —
// off the same compiler flag — so a constant cannot satisfy both arms.
//
// AOT, for the reason XpuCpuDotAccumTierTests records.

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include "../PortableEnv.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

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
              / ("cajeta_dmem_" + std::to_string(rng()));
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

// Total physical RAM through the SAME OS API the runtime's CPU arm uses,
// so agreement is exact — same host, same call, same instant.
int64_t hostTotalRam() {
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    return (int64_t) ms.ullTotalPhys;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || psize <= 0) return 0;
    return (int64_t) pages * (int64_t) psize;
#endif
}

// A backend is bundled only when the program HAS a kernel (the
// registration-ctor rule XpuActiveBackendTests records).
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
        System.stdout.println("mem=" + Device.memoryBytes());
    }
}
)CJ";

const char* NO_KERNEL = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
public class B {
    public static void run() {
        System.stdout.println("mem=" + Device.memoryBytes());
    }
}
)CJ";

int64_t memFrom(const std::string& out) {
    auto i = out.find("mem=");
    if (i == std::string::npos) return -1;
    return (int64_t) std::strtoll(out.c_str() + i + 4, nullptr, 10);
}

}  // namespace

// Built --xpu-backend=cpu, so the CPU backend is selected and the device
// is the host: the answer must equal total physical RAM, read through
// the same API by this very test.
TEST(XpuDeviceMemoryTests, cpuBackendReportsTotalPhysicalRamExactly) {
    const std::string out = buildAndRun(REPORT, "probe.B.run");
    const int64_t got = memFrom(out);
    const int64_t want = hostTotalRam();
    ASSERT_GT(want, 0) << "host RAM query failed in the test itself";
    EXPECT_EQ(got, want) << out;
}

// The other half of the contract, off the same compiler flag: no kernel
// bundles no backend, and 0 means UNKNOWN. A stub constant cannot
// produce both this and the arm above.
TEST(XpuDeviceMemoryTests, noBundledBackendAnswersZero) {
    const std::string out = buildAndRun(NO_KERNEL, "probe.B.run");
    EXPECT_EQ(memFrom(out), 0) << out;
}
