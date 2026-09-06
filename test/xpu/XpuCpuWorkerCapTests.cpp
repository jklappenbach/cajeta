// Bounding — and OBSERVING — the CPU backend's kernel parallelism.
//
// `nworkers = min(nblocks, cores)` with no way to ask for fewer and no way
// to find out what it picked. Both halves are needed by the
// threaded-forward-path scaling curve (plan 5.1.1, 5.2.1): a timing whose
// worker count is unknown is not a data point, and a sweep of 1/2/4/8/16
// workers cannot be run at all.
//
// The cap is settable IN-PROCESS, not only through the environment,
// because the measurement discipline this project settled on requires
// alternating the arm order within one run — a fixed arm order let a
// decaying background load make a mutex look like a speedup once already.
// A process-lifetime environment variable cannot alternate.
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
#include <thread>
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

std::string buildAndRun(const std::string& source, const char* entry,
                        std::string* buildLog = nullptr) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_wcap_" + std::to_string(rng()));
    fs::create_directories(base / "probe");
    fs::create_directories(base / "arch");
    std::ofstream(base / "probe" / "W.cajeta") << source;
    const std::string exe = (base / "out").string();
    const std::string cmd = "\"" + compilerPath() + "\""
        + " --release --emit=exe --xpu-backend=cpu"
        + " -o \"" + exe + "\" " + entry
        + " \"" + base.string() + "\" \"" + (base / "arch").string() + "\"";
    std::string log = capture(cmd + " 2>&1");
    if (buildLog) *buildLog = log;
    std::string out;
    if (fs::exists(exe)) out = capture("\"" + exe + "\"");
    std::error_code ec;
    fs::remove_all(base, ec);
    return out;
}

// 64 blocks of 64 work items: past the 256-work-item parallel threshold and
// past nblocks <= 1, so the launch genuinely fans out.
const char* SWEEP = R"CJ(
package probe;
import cajeta.lang.System;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.KernelStream;
public class W {
    @Kernel
    public static void touch(KernelBuffer<int32> out, uint32 n) {
        uint32 g = KernelThread.globalIdX();
        if (g < n) { out[g] = (int32) g * 2; }
    }
    static int32 runAt(int32 cap, uint32 n) {
        Device.setCpuWorkerCap(cap);
        int32[] ho #= heap int32[n];
        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);
        KernelStream s #= KernelStream.current();
        touch.launch(s, grid: [n / 64], block: [64])(out, n);
        s.sync();
        out.download(ho);
        // The answer must stay right at every worker count -- a cap that
        // dropped a slice would show up here, not in the count.
        uint32 i = 0;
        while (i < n) {
            if (ho[i] != (int32) i * 2) {
                System.stdout.println("WRONG at " + i);
                return -1;
            }
            i = i + 1;
        }
        return Device.lastLaunchWorkers();
    }
    public static void run() {
        uint32 n = 4096;
        System.stdout.println("uncapped " + W.runAt(0, n));
        System.stdout.println("cap1 " + W.runAt(1, n));
        System.stdout.println("cap2 " + W.runAt(2, n));
        System.stdout.println("cap4 " + W.runAt(4, n));
        System.stdout.println("cap8 " + W.runAt(8, n));
        // Back to unlimited, and it must recover -- a one-way cap would
        // silently pin every later launch in the process.
        System.stdout.println("again " + W.runAt(0, n));
        return;
    }
}
)CJ";

int readLabelled(const std::string& out, const std::string& label) {
    const size_t p = out.find(label + " ");
    if (p == std::string::npos) return -999;
    return std::atoi(out.c_str() + p + label.size() + 1);
}

}  // namespace

TEST(XpuCpuWorkerCapTests, capBoundsTheWorkerCountAndIsObservable) {
    std::string log;
    const std::string out = buildAndRun(SWEEP, "probe.W.run", &log);
    ASSERT_FALSE(out.empty())
        << "probe failed to build or produced no output. Build said:\n" << log;
    EXPECT_EQ(out.find("WRONG"), std::string::npos)
        << "a capped launch computed the wrong answer: " << out;

    EXPECT_EQ(readLabelled(out, "cap1"), 1) << out;
    EXPECT_EQ(readLabelled(out, "cap2"), 2) << out;
    EXPECT_EQ(readLabelled(out, "cap4"), 4) << out;
    EXPECT_EQ(readLabelled(out, "cap8"), 8) << out;
}

// The does-NOT-fire half. A `lastLaunchWorkers()` hard-wired to the cap, or
// a cap that never lifts, passes every assertion above.
TEST(XpuCpuWorkerCapTests, uncappedUsesMoreThanOneWorkerAndTheCapLifts) {
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2) GTEST_SKIP() << "single-core box: nothing to fan out to";

    const std::string out = buildAndRun(SWEEP, "probe.W.run");
    ASSERT_FALSE(out.empty()) << "probe failed to build";

    const int uncapped = readLabelled(out, "uncapped");
    EXPECT_GT(uncapped, 1)
        << "an uncapped launch of 64 blocks on a " << hw
        << "-core box ran on one worker; the sweep would then be measuring "
           "nothing. Got: " << out;
    EXPECT_EQ(readLabelled(out, "again"), uncapped)
        << "setting the cap back to 0 did not restore unlimited workers, so "
           "the cap is one-way and every later launch in the process is "
           "silently pinned. Got: " << out;
}
