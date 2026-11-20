//
// VulkanUnmappedBufferTests — a KernelBuffer that cannot get mappable device-local memory takes
// unmapped device-local memory, written and read through staging (gpu-kernel-parity 16.2.4.1).
//
#include "gtest/gtest.h"

#include "XpuDeviceTestUtil.h"
#include "../PortableEnv.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

// Uploads into a 4 MB buffer before and after a kernel reads it, round-trips a 256 B buffer, then
// downloads; prints 777 when every element matches.
const char* kSource = R"CJ(
package probe;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class U {
    @Kernel
    public static void affine(KernelBuffer<uint32> out, KernelBuffer<uint32> in, uint32 k) {
        uint32 g = KernelThread.globalIdX();
        out[g] = in[g] * 3 + k;
    }
    public static int32 main() {
        uint32 n = (uint32) 1048576;
        uint32[] h = heap uint32[n];
        uint32[] r = heap uint32[n];
        int32 i = 0;
        while (i < (int32) n) { h[i] = (uint32) (i * 7); i = i + 1; }
        KernelBuffer<uint32> x = heap KernelBuffer<uint32>((uint64) n);
        KernelBuffer<uint32> y = heap KernelBuffer<uint32>((uint64) n);
        KernelBuffer<uint32> z = heap KernelBuffer<uint32>((uint64) n);
        KernelStream s #= KernelStream.current();
        x.upload(h);
        affine.launch(s, grid: [n / 256], block: [256])(y, x, 1);
        i = 0;
        while (i < (int32) n) { h[i] = (uint32) (i * 5); i = i + 1; }
        x.upload(h);
        affine.launch(s, grid: [n / 256], block: [256])(z, x, 2);
        s.sync();
        y.download(r);
        i = 0;
        while (i < (int32) n) {
            if (r[i] != (uint32) (i * 7) * 3 + 1) { System.stdout.println("y wrong at " + i); return 1; }
            i = i + 1;
        }
        uint32[] hs = heap uint32[64];
        i = 0;
        while (i < 64) { hs[i] = (uint32) (i + 9); i = i + 1; }
        KernelBuffer<uint32> w = heap KernelBuffer<uint32>((uint64) 64);
        KernelBuffer<uint32> v = heap KernelBuffer<uint32>((uint64) 64);
        w.upload(hs);
        affine.launch(s, grid: [1], block: [64])(v, w, 4);
        s.sync();
        v.download(hs);
        i = 0;
        while (i < 64) {
            if (hs[i] != (uint32) (i + 9) * 3 + 4) { System.stdout.println("v wrong at " + i); return 1; }
            i = i + 1;
        }
        z.download(r);
        i = 0;
        while (i < (int32) n) {
            if (r[i] != (uint32) (i * 5) * 3 + 2) { System.stdout.println("z wrong at " + i); return 1; }
            i = i + 1;
        }
        System.stdout.println("result 777");
        return 0;
    }
}
)CJ";

std::string sourceRoot() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    if (root && *root) return root;
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
    return CAJETA_SOURCE_ROOT_DEFAULT;
#else
    return ".";
#endif
}

std::string run(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cajeta_shell(cmd + " 2>&1").c_str(), "r");
    if (!p) return out;
    std::array<char, 512> buf;
    while (fgets(buf.data(), (int) buf.size(), p)) out += buf.data();
    pclose(p);
    return out;
}

// Builds the probe once per process; the executable's path, or "" with the build log in `log`.
std::string buildProbe(std::string& log) {
    static std::string exe, built;
    if (exe.empty()) {
        static std::mt19937_64 rng(std::random_device{}());
        fs::path base = fs::temp_directory_path() / ("cajeta_vkunmapped_" + std::to_string(rng()));
        fs::create_directories(base / "src" / "probe");
        fs::create_directories(base / "out");
        std::ofstream(base / "src" / "probe" / "U.cajeta") << kSource;
        const std::string outBase = (base / "out" / "u").string();
        built = run("\"" + sourceRoot() + "/build/src/cajeta\" --emit=exe --xpu-backend=vulkan -o \"" + outBase +
                    "\" probe.U.main \"" + (base / "src").string() + "\" \"" + (base / "out").string() + "\"");
        if (fs::exists(cajeta_exe_path(outBase))) exe = cajeta_exe_path(outBase).string();
    }
    log = built;
    return exe;
}

// The probe's output with the allocation log on, plus `extra` in its environment.
std::string runProbe(std::initializer_list<std::pair<const char*, std::string>> extra) {
    std::string log;
    std::string exe = buildProbe(log);
    if (exe.empty()) return "BUILD FAILED\n" + log;
    std::string env = cajeta_env_prefix({{"CAJETA_XPU_BACKEND", "vulkan"}, {"CAJETA_XPU_VK_ALLOC_LOG", "1"},
                                          {"CAJETA_XPU_VK_UNMAPPED", "0"}, {"CAJETA_XPU_VK_SMALL_BAR", "0"}});
    return run(env + cajeta_env_prefix(extra) + "\"" + exe + "\"");
}

} // namespace

// The allocation-log lines for kernel buffers of `bytes`, each checked for `want`; the line count.
int expectKernelLines(const std::string& out, const std::string& bytes, const std::string& want) {
    int lines = 0;
    size_t at = 0;
    const std::string key = "kernel " + bytes + " B -> memory type";
    while ((at = out.find(key, at)) != std::string::npos) {
        std::string line = out.substr(at, out.find('\n', at) - at);
        EXPECT_NE(line.find(want), std::string::npos) << line;
        ++lines;
        ++at;
    }
    return lines;
}

// Forced onto unmapped device-local memory, every kernel buffer is unmapped and every value round-trips.
TEST(VulkanUnmappedBufferDeviceTests, forcedUnmappedBuffersRoundTripOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    std::string out = runProbe({{"CAJETA_XPU_VK_UNMAPPED", "1"}});
    EXPECT_NE(out.find("result 777"), std::string::npos) << out;
    EXPECT_EQ(expectKernelLines(out, "4194304", "device-local 1, mapped 0"), 3) << out;
    EXPECT_EQ(expectKernelLines(out, "256", "device-local 1, mapped 0"), 2) << out;
}

// With a small BAR window, buffers over 1 MB skip it and small ones stay mapped.
TEST(VulkanUnmappedBufferDeviceTests, aSmallBarKeepsItsWindowForSmallBuffers) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    std::string out = runProbe({{"CAJETA_XPU_VK_SMALL_BAR", "1"}});
    EXPECT_NE(out.find("result 777"), std::string::npos) << out;
    EXPECT_EQ(expectKernelLines(out, "4194304", "device-local 1, mapped 0"), 3) << out;
    EXPECT_EQ(expectKernelLines(out, "256", "mapped 1"), 2) << out;
}

// Without the switch the same program still round-trips (mapped wherever the device allows).
TEST(VulkanUnmappedBufferDeviceTests, defaultBuffersRoundTripOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    std::string out = runProbe({});
    EXPECT_NE(out.find("result 777"), std::string::npos) << out;
}
