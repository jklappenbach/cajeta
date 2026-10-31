// cpu-isa-variants Unit 1: the cpu backend's ISA levels and the kernel resolver.
//
// A `--cpu=portable` program registers every kernel once per level; at startup
// the runtime picks the highest level the host supports. These tests drive the
// registry through its C entry points with fake kernel pointers, so they need no
// compile and run on whatever level this box has: expectations are written
// against the detected level, and an override that only LOWERS the level stands
// in for the lesser hosts proton cannot be.
#include "gtest/gtest.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

extern "C" {
void __cajeta_xpu_register_backend(int32_t id);
int32_t __cajeta_xpu_force_backend(int32_t id);
int64_t __cajeta_xpu_device_geometry(int32_t key);
int32_t __cajeta_xpu_cpu_host_level(void);
void __cajeta_xpu_cpu_set_level_override(int32_t level);
const char* __cajeta_xpu_cpu_level_name(int32_t level);
void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn);
void __cajeta_xpu_register_cpu_kernel_at(const char* name, int32_t level, void* fn);
void* __cajeta_xpu_lookup_cpu_kernel(const char* name);
int32_t __cajeta_xpu_cpu_kernel_level(const char* name);
int32_t __cajeta_xpu_cpu_resolved_level(void);
void __cajeta_xpu_set_cpu_wave_at(int32_t level, int32_t lanes);
const char* __cajeta_xpu_cpu_level_refusal(void);
void __cajeta_xpu_register_kernel_manifest(const char* kernelName, int32_t backend, const char* arch,
                                           const void* json, uint64_t len);
void* __cajeta_xpu_kernel_manifest_json(void* nameArr, int64_t len);
}

namespace {
constexpr int32_t kNone = -1, kV3 = 0, kV3Vnni = 1, kV4 = 2, kV4Amx = 3, kHost = 100;
constexpr int32_t kClear = -2;
constexpr int32_t kBackendCpu = 3;
constexpr int32_t kWaveSize = 2;   // CajetaXpuGeometryKey ordinal

// Fake kernels: the registry stores and returns pointers, never calls them.
int fnA, fnB, fnC;

bool cpuinfoHas(const char* flag) {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    const std::string needle = std::string(" ") + flag;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) != 0) continue;
        line += " ";
        return line.find(needle + " ") != std::string::npos;
    }
    return false;
}

struct ClearOverride {
    ~ClearOverride() { __cajeta_xpu_cpu_set_level_override(kClear); }
};
}  // namespace

// 1.1.1: the detected level agrees with the host's flags, and the names round-trip.
TEST(XpuCpuLevel, theHostLevelFollowsCpuid) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
#if defined(__x86_64__)
    if (!cpuinfoHas("avx2")) {
        EXPECT_EQ(host, kNone);
        return;
    }
    EXPECT_GE(host, kV3);
    const bool v4 = cpuinfoHas("avx512_vnni") && cpuinfoHas("avx512bw") && cpuinfoHas("avx512vl");
    const bool vnni = cpuinfoHas("avx_vnni");
    if (v4) EXPECT_GE(host, kV4) << "avx512_vnni on this box";
    else if (vnni) EXPECT_EQ(host, kV3Vnni);
    else EXPECT_EQ(host, kV3);
    if (!cpuinfoHas("amx_int8")) EXPECT_NE(host, kV4Amx);
#else
    EXPECT_EQ(host, kNone) << "levels are x86's for now";
#endif
    EXPECT_STREQ(__cajeta_xpu_cpu_level_name(kV3), "v3");
    EXPECT_STREQ(__cajeta_xpu_cpu_level_name(kV3Vnni), "v3-vnni");
    EXPECT_STREQ(__cajeta_xpu_cpu_level_name(kV4), "v4");
    EXPECT_STREQ(__cajeta_xpu_cpu_level_name(kV4Amx), "v4-amx");
    EXPECT_STREQ(__cajeta_xpu_cpu_level_name(kHost), "host");
}

// 1.1.1: the override only lowers the level; it cannot claim features the host lacks.
TEST(XpuCpuLevel, theOverrideLowersAndNeverRaises) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
    if (host < kV3) GTEST_SKIP() << "no level on this box";
    __cajeta_xpu_cpu_set_level_override(kV3);
    EXPECT_EQ(__cajeta_xpu_cpu_host_level(), kV3);
    __cajeta_xpu_cpu_set_level_override(kV4Amx);
    EXPECT_EQ(__cajeta_xpu_cpu_host_level(), host) << "an override above the host is clamped";
    __cajeta_xpu_cpu_set_level_override(kClear);
    EXPECT_EQ(__cajeta_xpu_cpu_host_level(), host);
}

// 1.1.2: a name at several levels resolves to the highest the host supports, and a
// level the host lacks steps down to the next the name has.
TEST(XpuCpuLevel, aNameResolvesToTheHighestSupportedLevel) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
    if (host < kV3) GTEST_SKIP() << "no level on this box";
    __cajeta_xpu_register_cpu_kernel_at("lvl.t1", kV3, &fnA);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t1", kV4, &fnB);
    if (host >= kV4) {
        EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t1"), &fnB);
        EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t1"), kV4);
    } else {
        EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t1"), &fnA);
        EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t1"), kV3);
    }
    // No v3-vnni copy: a v3-vnni host takes the v3 one.
    __cajeta_xpu_cpu_set_level_override(kV3Vnni);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t1"), &fnA);
    EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t1"), kV3);
    __cajeta_xpu_cpu_set_level_override(kV3);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t1"), &fnA);
}

// 1.1.3: a name registered only above the host's level is refused by name, with the
// host's level and the lowest built in the message, and nothing runs in its place.
TEST(XpuCpuLevel, aLevelAboveTheHostIsRefusedByName) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
    if (host < kV3) GTEST_SKIP() << "no level on this box";
    // Force a v3 host so the v4 copy is out of reach on any box.
    __cajeta_xpu_cpu_set_level_override(kV3);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t2", kV4, &fnB);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t2", kV4Amx, &fnC);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t2"), nullptr);
    EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t2"), kNone);
    const std::string why = __cajeta_xpu_cpu_level_refusal();
    EXPECT_NE(why.find("lvl.t2"), std::string::npos) << why;
    EXPECT_NE(why.find("v3"), std::string::npos) << "the host's level: " << why;
    EXPECT_NE(why.find("v4"), std::string::npos) << "the lowest built: " << why;
}

// 1.1.2: today's registration is level `host` and always resolves; a level copy
// beside it is preferred when the host reaches it.
TEST(XpuCpuLevel, aHostRegistrationAlwaysResolves) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    __cajeta_xpu_register_cpu_kernel("lvl.t3", &fnA);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t3"), &fnA);
    EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t3"), kHost);
    if (__cajeta_xpu_cpu_host_level() < kV3) return;
    __cajeta_xpu_register_cpu_kernel_at("lvl.t3", kV3, &fnB);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t3"), &fnB);
    EXPECT_EQ(__cajeta_xpu_cpu_kernel_level("lvl.t3"), kV3);
    // Re-registering a level replaces that level's copy, as `host` always did.
    __cajeta_xpu_register_cpu_kernel_at("lvl.t3", kV3, &fnC);
    EXPECT_EQ(__cajeta_xpu_lookup_cpu_kernel("lvl.t3"), &fnC);
}

// 2.4 (runtime half): the cpu wave is the resolved level's.
TEST(XpuCpuLevel, theWaveFollowsTheResolvedLevel) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
    if (host < kV4) GTEST_SKIP() << "needs a v4 host to resolve two levels";
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t4", kV3, &fnA);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t4", kV4, &fnB);
    __cajeta_xpu_set_cpu_wave_at(kV3, 8);
    __cajeta_xpu_set_cpu_wave_at(kV4, 16);
    EXPECT_EQ(__cajeta_xpu_cpu_resolved_level(), kV4);
    EXPECT_EQ(__cajeta_xpu_device_geometry(kWaveSize), 16);
    __cajeta_xpu_cpu_set_level_override(kV3);
    EXPECT_EQ(__cajeta_xpu_cpu_resolved_level(), kV3);
    EXPECT_EQ(__cajeta_xpu_device_geometry(kWaveSize), 8);
    __cajeta_xpu_cpu_set_level_override(kClear);
    // Leave the built wave as the host's for the tests after this one.
    __cajeta_xpu_set_cpu_wave_at(kV3, 0);
    __cajeta_xpu_set_cpu_wave_at(kV4, 0);
}

// 4.3.2: with one manifest per level copy, KernelManifest.of(name) answers the copy
// the host resolved to, not the last one registered.
TEST(XpuCpuLevel, theManifestIsTheResolvedCopys) {
    ClearOverride c;
    __cajeta_xpu_cpu_set_level_override(kClear);
    const int32_t host = __cajeta_xpu_cpu_host_level();
    if (host < kV4) GTEST_SKIP() << "needs a v4 host to resolve two levels";
    __cajeta_xpu_register_backend(kBackendCpu);
    __cajeta_xpu_force_backend(kBackendCpu);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t5", kV3, &fnA);
    __cajeta_xpu_register_cpu_kernel_at("lvl.t5", kV4, &fnB);
    static const char j8[] = "{\"waveWidth\":8}";
    static const char j16[] = "{\"waveWidth\":16}";
    __cajeta_xpu_register_kernel_manifest("lvl.t5", kBackendCpu, "v3", j8, sizeof(j8) - 1);
    __cajeta_xpu_register_kernel_manifest("lvl.t5", kBackendCpu, "v4", j16, sizeof(j16) - 1);
    char nameArr[8 + 8] = {};
    memcpy(nameArr + 8, "lvl.t5", 6);
    auto jsonOf = [&]() -> std::string {
        void* hdr = __cajeta_xpu_kernel_manifest_json(nameArr, 6);
        if (!hdr) return "";
        const char* p = (const char*) hdr + 8;
        std::string s(p, strnlen(p, 64));
        return s;
    };
    EXPECT_EQ(jsonOf().rfind(j16, 0), 0u) << "resolved v4: " << jsonOf();
    __cajeta_xpu_cpu_set_level_override(kV3);
    EXPECT_EQ(jsonOf().rfind(j8, 0), 0u) << "forced v3: " << jsonOf();
    __cajeta_xpu_cpu_set_level_override(kClear);
}
