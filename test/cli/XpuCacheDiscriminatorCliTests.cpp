// The incremental cache discriminator must separate device backends.
//
// xpu-cache-discriminator-spec §2.1-§2.4, §5.1. MEASURED 2026-09-02: five
// materially different builds — cpu, amdgpu/gfx1151, nvptx, sm_89, and no xpu
// flags at all — all returned ONE key. A project built for one accelerator and
// then another therefore reused the first's device objects, and the second
// binary shipped kernels for a backend it was not asked for while reporting
// that backend at runtime.
//
// These tests need no accelerator and compile nothing: --print-cache-
// discriminator resolves flags and prints the key (§5.1). That is deliberate —
// the defect is in flag resolution, and a test that needed a device could not
// run on the box where the bug was found.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "../PortableEnv.h"

namespace fs = std::filesystem;

namespace {

std::string discriminatorCompilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

// The key for one flag set. Empty string on failure, so a broken invocation
// cannot masquerade as "two builds agree".
std::string keyFor(const std::string& xpuFlags) {
    static std::mt19937_64 rng(std::random_device{}());
    fs::path log = fs::temp_directory_path()
                 / ("cajeta_disc_" + std::to_string(rng()) + ".log");
    std::string cmd = "\"" + discriminatorCompilerBinary() + "\" " + xpuFlags
                    + " --print-cache-discriminator > \"" + log.string()
                    + "\" 2>&1";
    int rc = std::system(cajeta_shell(cmd).c_str());
    std::ifstream in(log);
    std::string first;
    std::getline(in, first);
    in.close();
    std::error_code ec;
    fs::remove(log, ec);
    if (rc != 0) return {};
    // The hash is the first line; a hint line may follow.
    while (!first.empty() && (first.back() == '\r' || first.back() == '\n'))
        first.pop_back();
    // Guard against a hint line being mistaken for a key.
    if (first.size() != 64) return {};
    return first;
}

}  // namespace

// 1.1.1 — the defect itself. RED before the fix: all three measured
// 1fbd8964aeebc54ee92229ced8c4e543c06f471e5a341d7ba43b96023958be52.
TEST(XpuCacheDiscriminatorCliTests, discriminatorDiffersByXpuBackend) {
    const std::string cpu    = keyFor("--xpu-backend=cpu");
    const std::string amdgpu = keyFor("--xpu-backend=amdgpu");
    const std::string nvptx  = keyFor("--xpu-backend=nvptx");

    ASSERT_EQ(64u, cpu.size())    << "no key for --xpu-backend=cpu";
    ASSERT_EQ(64u, amdgpu.size()) << "no key for --xpu-backend=amdgpu";
    ASSERT_EQ(64u, nvptx.size())  << "no key for --xpu-backend=nvptx";

    std::set<std::string> distinct{cpu, amdgpu, nvptx};
    EXPECT_EQ(3u, distinct.size())
        << "three backends must key three ways, got:\n"
        << "  cpu    " << cpu << "\n"
        << "  amdgpu " << amdgpu << "\n"
        << "  nvptx  " << nvptx;
}

// 1.1.2 — the arch is as load-bearing as the backend: gfx1151 and gfx1100 are
// different ISAs, sm_89 and sm_90 different SM targets.
TEST(XpuCacheDiscriminatorCliTests, discriminatorDiffersByXpuArch) {
    const std::string gfx1151 = keyFor("--xpu-backend=amdgpu --xpu-arch=gfx1151");
    const std::string gfx1100 = keyFor("--xpu-backend=amdgpu --xpu-arch=gfx1100");
    const std::string sm89    = keyFor("--xpu-backend=nvptx --xpu-arch=sm_89");
    const std::string sm90    = keyFor("--xpu-backend=nvptx --xpu-arch=sm_90");

    ASSERT_EQ(64u, gfx1151.size());
    ASSERT_EQ(64u, sm89.size());

    EXPECT_NE(gfx1151, gfx1100) << "gfx1151 and gfx1100 share a key: " << gfx1151;
    EXPECT_NE(sm89, sm90)       << "sm_89 and sm_90 share a key: " << sm89;
}

// 1.1.3 — §2.3. "No device kernels at all" and "CPU kernels embedded" are
// different outputs, so an absent backend must not resolve to the cpu string.
TEST(XpuCacheDiscriminatorCliTests, hostOnlyIsNotTheSameAsCpuKernels) {
    const std::string hostOnly = keyFor("");
    const std::string cpu      = keyFor("--xpu-backend=cpu");

    ASSERT_EQ(64u, hostOnly.size());
    ASSERT_EQ(64u, cpu.size());
    EXPECT_NE(hostOnly, cpu)
        << "host-only and --xpu-backend=cpu share a key: " << hostOnly;
}

// 1.1.4 — THE CONTROL. Without it, a fix that salted every build with a
// timestamp would pass every test above and destroy incrementality (§2.4).
TEST(XpuCacheDiscriminatorCliTests, theSameFlagsStillAgree) {
    const std::string a = keyFor("--xpu-backend=amdgpu --xpu-arch=gfx1151");
    const std::string b = keyFor("--xpu-backend=amdgpu --xpu-arch=gfx1151");

    ASSERT_EQ(64u, a.size());
    EXPECT_EQ(a, b) << "identical flag sets must key identically";
}

// 1.2.1's contract, as a test: --xpu-backend takes a LIST, and a set of
// backends is what it names. `amdgpu,cpu` and `cpu,amdgpu` bundle the same
// device code and must not key differently — otherwise the fix trades a
// false-share for a false-miss and every reordered flag rebuilds the world.
TEST(XpuCacheDiscriminatorCliTests, backendListOrderDoesNotChangeTheKey) {
    const std::string ac = keyFor("--xpu-backend=amdgpu,cpu");
    const std::string ca = keyFor("--xpu-backend=cpu,amdgpu");

    ASSERT_EQ(64u, ac.size()) << "no key for --xpu-backend=amdgpu,cpu";
    ASSERT_EQ(64u, ca.size()) << "no key for --xpu-backend=cpu,amdgpu";
    EXPECT_EQ(ac, ca) << "backend list order must not change the key:\n"
                      << "  amdgpu,cpu " << ac << "\n"
                      << "  cpu,amdgpu " << ca;
}

// A multi-backend bundle is not the same artifact as either of its members.
TEST(XpuCacheDiscriminatorCliTests, aBundleDiffersFromItsMembers) {
    const std::string bundle = keyFor("--xpu-backend=amdgpu,cpu");
    const std::string amdgpu = keyFor("--xpu-backend=amdgpu");
    const std::string cpu    = keyFor("--xpu-backend=cpu");

    ASSERT_EQ(64u, bundle.size());
    EXPECT_NE(bundle, amdgpu) << "bundle shares a key with amdgpu alone";
    EXPECT_NE(bundle, cpu)    << "bundle shares a key with cpu alone";
}
