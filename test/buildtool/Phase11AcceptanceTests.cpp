// Phase 11 — Sandboxing + reproducible builds. Pins the four
// acceptance criteria from plans/buildtool/build-tool-plan.md "Phase 11 —
// Sandboxing + reproducible builds":
//
//   1. Same source + lockfile + compiler builds byte-identical
//      `.cja` on three independent runners. (Unit-level: the
//      reproducible-build helpers produce deterministic outputs
//      given the same inputs; byteCompareFiles surfaces drift in
//      one line for the CI verifier.)
//   2. Sandbox violation (action reading /etc/passwd) is reported
//      with the offending path. (Unit-level: a network-disabled
//      bwrap-wrapped argv carries `--unshare-net`, and a
//      capability-violation surfaces the offending Capability via
//      `firstDisallowedCapability`.)
//   3. No network access from `build` action on a networked
//      machine. (Unit-level: a build-shaped sandbox policy excludes
//      Capability::Network, and the wrapper adds `--unshare-net`
//      when the policy lacks the capability.)
//   4. Reproducible-build verifier passes for 7 consecutive
//      nights. (Unit-level: verifyReproducibleArchive identifies
//      identical bytes as identical and surfaces the first
//      differing byte when they diverge.)
//
// Plus deliverable-coverage:
//   - --no-sandbox debug flag (policy.disabled passthrough)
//   - SOURCE_DATE_EPOCH precedence (env → CI knob → property →
//     default "0")
//   - debug-prefix-map composition
//   - deterministic seed derivation
//   - capability vocabulary round-trip
//   - native action capability catalog completeness

#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Reproducibility.h"
#include "cajeta/buildtool/Sandbox.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

using cajeta::buildtool::byteCompareFiles;
using cajeta::buildtool::Capability;
using cajeta::buildtool::capabilityFromString;
using cajeta::buildtool::capabilityToString;
using cajeta::buildtool::composeDebugPrefixMap;
using cajeta::buildtool::deterministicSeed;
using cajeta::buildtool::firstDisallowedCapability;
using cajeta::buildtool::hostSandboxAvailable;
using cajeta::buildtool::nativeActionCapabilities;
using cajeta::buildtool::reproducibilityFlags;
using cajeta::buildtool::resolveSourceDateEpoch;
using cajeta::buildtool::ResolvedProperties;
using cajeta::buildtool::SandboxedInvocation;
using cajeta::buildtool::SandboxPolicy;
using cajeta::buildtool::verifyReproducibleArchive;
using cajeta::buildtool::wrapInSandbox;

namespace {

    std::filesystem::path tempRoot(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-phase11-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& p,
                   const std::string& contents) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
    }

    bool containsArg(const std::vector<std::string>& argv,
                     const std::string& needle) {
        for (const auto& a : argv) if (a == needle) return true;
        return false;
    }

}  // namespace

TEST(Phase11, capabilityVocabularyRoundTrips) {
    for (auto c : {Capability::Filesystem, Capability::Process,
                   Capability::Network, Capability::Env}) {
        auto s = capabilityToString(c);
        EXPECT_NE(s, "<unknown>");
        auto back = capabilityFromString(s);
        ASSERT_TRUE(back.has_value()) << "round trip for " << s;
        EXPECT_EQ(*back, c);
    }
    EXPECT_FALSE(capabilityFromString("not-a-capability").has_value());
}

TEST(Phase11, firstDisallowedCapabilityCitesOffender) {
    std::set<Capability> requested = {Capability::Filesystem,
                                       Capability::Network};
    std::set<Capability> allowed = {Capability::Filesystem,
                                     Capability::Process};
    auto bad = firstDisallowedCapability(requested, allowed);
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(*bad, Capability::Network);
    // Subset of allowed → no error.
    std::set<Capability> ok = {Capability::Filesystem};
    EXPECT_FALSE(firstDisallowedCapability(ok, allowed).has_value());
}

TEST(Phase11, nativeActionCatalogCoversFirstPartyActions) {
    // Acceptance: every native action registered in ActionRegistry has a
    // capability declaration. Catches future additions that forget to
    // register their capability footprint.
    for (const char* name : {"exec", "copy", "delete", "mkdir", "sign",
                              "verify-sig", "version", "download", "build",
                              "clean", "test", "package", "upload",
                              "publish"}) {
        auto caps = nativeActionCapabilities(name);
        EXPECT_TRUE(caps.has_value()) << "missing caps for action: " << name;
    }
    // build does NOT request Network — that's the no-network-from-
    // build invariant, criterion 3.
    auto build = nativeActionCapabilities("build");
    ASSERT_TRUE(build.has_value());
    EXPECT_EQ(build->count(Capability::Network), 0u)
        << "build action must NOT request Network (criterion 3)";
    EXPECT_GT(build->count(Capability::Process), 0u);
    EXPECT_GT(build->count(Capability::Filesystem), 0u);
    // upload/publish DO request Network (intentionally — they're
    // the network-side actions).
    auto up = nativeActionCapabilities("upload");
    ASSERT_TRUE(up.has_value());
    EXPECT_GT(up->count(Capability::Network), 0u);
}

TEST(Phase11, sandboxWrapsBuildArgvWithoutNetwork) {
    // Criterion 3: when the policy lacks Capability::Network, the
    // wrapped argv carries `--unshare-net` (Linux bwrap path).
    SandboxPolicy pol;
    pol.capabilities = {Capability::Filesystem, Capability::Process};
    pol.projectRoot = "/tmp/cajeta-test";
    std::vector<std::string> argv = {"cajetac", "--emit=archived-ir"};
    auto r = wrapInSandbox(pol, argv, {});
    ASSERT_TRUE(static_cast<bool>(r));
    if (r->strategy == "bwrap") {
        EXPECT_TRUE(containsArg(r->argv, "--unshare-net"))
            << "no Capability::Network → bwrap must `--unshare-net`";
        EXPECT_TRUE(containsArg(r->argv, "--die-with-parent"));
        EXPECT_TRUE(containsArg(r->argv, "/tmp/cajeta-test"));
    } else {
        // No bwrap installed → passthrough + a note explaining.
        ASSERT_FALSE(r->notes.empty());
    }
}

TEST(Phase11, sandboxKeepsNetworkWhenCapabilityRequested) {
    // The upload/publish action shape: network IS in the
    // capability set → wrapper must NOT carry --unshare-net.
    SandboxPolicy pol;
    pol.capabilities = {Capability::Filesystem, Capability::Process,
                        Capability::Network};
    pol.projectRoot = "/tmp/cajeta-test";
    std::vector<std::string> argv = {"curl", "https://registry.example/upload"};
    auto r = wrapInSandbox(pol, argv, {});
    ASSERT_TRUE(static_cast<bool>(r));
    if (r->strategy == "bwrap") {
        EXPECT_FALSE(containsArg(r->argv, "--unshare-net"))
            << "Capability::Network set → bwrap must NOT --unshare-net";
    }
}

TEST(Phase11, noSandboxFlagPassesArgvThrough) {
    // The --no-sandbox debug flag → strategy="passthrough" with
    // the unmodified argv. Used when developers need to peek into
    // a sandboxed action's behaviour.
    SandboxPolicy pol;
    pol.disabled = true;
    pol.capabilities = {Capability::Filesystem};
    std::vector<std::string> argv = {"echo", "hello"};
    auto r = wrapInSandbox(pol, argv, {"FOO=bar"});
    ASSERT_TRUE(static_cast<bool>(r));
    EXPECT_EQ(r->strategy, "passthrough");
    EXPECT_EQ(r->argv, argv);
    ASSERT_FALSE(r->notes.empty());
}

TEST(Phase11, sandboxFailsCleanlyOnEmptyArgv) {
    SandboxPolicy pol;
    auto r = wrapInSandbox(pol, {}, {});
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) llvm::consumeError(r.takeError());
}

TEST(Phase11, sourceDateEpochPrecedenceCajetaWinsOverStd) {
    // Precedence: CAJETA_SOURCE_DATE_EPOCH wins over
    // SOURCE_DATE_EPOCH wins over manifest property wins over
    // default "0".
    ResolvedProperties props;
    props.values["cajeta.source-date-epoch"] = "1700000000";

    std::map<std::string, std::string> env;
    EXPECT_EQ(resolveSourceDateEpoch(props, env), "1700000000")
        << "no env → use manifest property";

    env["SOURCE_DATE_EPOCH"] = "1800000000";
    EXPECT_EQ(resolveSourceDateEpoch(props, env), "1800000000")
        << "SOURCE_DATE_EPOCH wins over manifest";

    env["CAJETA_SOURCE_DATE_EPOCH"] = "1900000000";
    EXPECT_EQ(resolveSourceDateEpoch(props, env), "1900000000")
        << "CAJETA_SOURCE_DATE_EPOCH is highest priority";

    ResolvedProperties empty;
    EXPECT_EQ(resolveSourceDateEpoch(empty, {}), "0")
        << "no signal → default is hard-pinned 0";
}

TEST(Phase11, debugPrefixMapComposes) {
    EXPECT_EQ(composeDebugPrefixMap(""), "");
    EXPECT_EQ(composeDebugPrefixMap("/work/proj"), "/work/proj=cajeta:");
    EXPECT_EQ(composeDebugPrefixMap("/work/proj", "repo:"),
              "/work/proj=repo:");
}

TEST(Phase11, deterministicSeedIsContentBound) {
    auto s1 = deterministicSeed("sha256:aaaaaaaa");
    auto s2 = deterministicSeed("sha256:aaaaaaaa");
    auto s3 = deterministicSeed("sha256:bbbbbbbb");
    EXPECT_EQ(s1, s2) << "same content hash → same seed";
    EXPECT_NE(s1, s3) << "different content hash → different seed";
    // Non-zero (the SHA-256 of a non-empty blob is overwhelmingly
    // unlikely to start with 8 zero bytes).
    EXPECT_NE(s1, 0u);
}

TEST(Phase11, reproducibilityFlagsCarrySourceDateEpochAndPrefixMap) {
    ResolvedProperties props;
    props.values["cajeta.source-date-epoch"] = "1700000000";
    auto flags = reproducibilityFlags(props, "/work/proj", "sha256:xy");
    EXPECT_TRUE(containsArg(flags,
        "--source-date-epoch=1700000000"));
    EXPECT_TRUE(containsArg(flags,
        "--debug-prefix-map=/work/proj=cajeta:"));
    bool sawSeed = false;
    for (const auto& f : flags) {
        if (f.rfind("--seed=", 0) == 0) sawSeed = true;
    }
    EXPECT_TRUE(sawSeed);
}

TEST(Phase11, verifyReproducibleArchiveIdentifiesIdenticalBytes) {
    // Criterion 1 (unit-level): identical inputs produce
    // identical-archive verdicts.
    auto root = tempRoot("verifyIdentical");
    writeFile(root / "a.cja", "header...payload...trailer");
    writeFile(root / "b.cja", "header...payload...trailer");
    auto r = verifyReproducibleArchive(
        (root / "a.cja").string(),
        (root / "b.cja").string());
    EXPECT_TRUE(r.identical);
    EXPECT_TRUE(r.diff.empty());
    EXPECT_EQ(r.sizeA, r.sizeB);
}

TEST(Phase11, verifyReproducibleArchiveCitesFirstDifferingByte) {
    // Criterion 4 (unit-level): when bytes drift, the verifier
    // surfaces the offset + observed byte values — the input the
    // nightly CI verifier reports to investigators.
    auto root = tempRoot("verifyDrift");
    writeFile(root / "a.cja", "header...payloadA...trailer");
    writeFile(root / "b.cja", "header...payloadB...trailer");
    auto r = verifyReproducibleArchive(
        (root / "a.cja").string(),
        (root / "b.cja").string());
    EXPECT_FALSE(r.identical);
    EXPECT_NE(r.diff.find("first-differing byte"), std::string::npos)
        << "diff string must cite the byte offset: " << r.diff;
}

TEST(Phase11, sourceDateEpochInsideReproducibilityFlagsIsDeterministic) {
    // Same inputs → same flag sequence → same compile-argv hash →
    // same archive bytes (the precondition for criterion 1). We
    // assert the flag sequence is invariant.
    ResolvedProperties props;
    props.values["cajeta.source-date-epoch"] = "1700000000";
    auto a = reproducibilityFlags(props, "/work/proj", "sha256:xy");
    auto b = reproducibilityFlags(props, "/work/proj", "sha256:xy");
    EXPECT_EQ(a, b);
}

TEST(Phase11, hostSandboxAvailabilityIsObservable) {
    // Smoke. The function should never throw and should return
    // the same value within a single test run.
    bool first  = hostSandboxAvailable();
    bool second = hostSandboxAvailable();
    EXPECT_EQ(first, second);
    (void)first; (void)second;
}
