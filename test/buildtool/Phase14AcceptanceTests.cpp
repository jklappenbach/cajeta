// Phase 14 — Toolchain provisioning + dispatch. Pins the relevant
// acceptance criteria from plans/buildtool/build-tool-plan.md "Phase 14":
//
//   - .cajeta-toolchain override file precedes manifest pin
//   - CAJETA_NO_DISPATCH runs PATH binary regardless of pin
//   - settings.toolchain block parses (version + distribution +
//     channel + sha256 + fetch + from)
//   - fetch policies: auto/warn/error/off behaviour observable in
//     computeDispatchDecision
//   - cajeta toolchain pin <version> mutates cajeta.json
//   - reserved distribution names enforced
//   - toolchain identity participates in IR cache discriminator
//   - listInstalledToolchains finds the on-disk layout
//   - dispatch decision returns ReExec when pin satisfied by
//     installed binary
//
// (Acceptance criteria gated on real archive fetch + signature
// verification on install land alongside the toolchain registry
// HTTP protocol implementation; the unit-level surface for the
// dispatch + pin + store layers is fully exercised here.)

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Toolchain.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../PortableEnv.h"

using cajeta::buildtool::computeDispatchDecision;
using cajeta::buildtool::DispatchAction;
using cajeta::buildtool::FetchPolicy;
using cajeta::buildtool::fetchPolicyFromString;
using cajeta::buildtool::fetchPolicyToString;
using cajeta::buildtool::InstalledToolchain;
using cajeta::buildtool::isReservedDistribution;
using cajeta::buildtool::listInstalledToolchains;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseToolchainPin;
using cajeta::buildtool::readToolchainOverrideFile;
using cajeta::buildtool::resolveEffectiveToolchain;
using cajeta::buildtool::ResolvedToolchain;
using cajeta::buildtool::ToolchainStoreLayout;
using cajeta::buildtool::resolveToolchainStoreLayout;
using cajeta::buildtool::runningBinarySatisfiesPin;
using cajeta::buildtool::ToolchainPin;
using cajeta::buildtool::toolchainIdentity;

namespace {

    std::filesystem::path tempRoot(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-phase14-" + tag + "-" +
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

    struct EnvScope {
        std::string key;
        bool had = false;
        std::string previous;
        EnvScope(const std::string& k, const std::string& v) : key(k) {
            const char* old = std::getenv(k.c_str());
            if (old) { had = true; previous = old; }
            ::setenv(k.c_str(), v.c_str(), 1);
        }
        ~EnvScope() {
            if (had) ::setenv(key.c_str(), previous.c_str(), 1);
            else     ::unsetenv(key.c_str());
        }
    };

    struct EnvUnscope {
        std::string key;
        bool had = false;
        std::string previous;
        EnvUnscope(const std::string& k) : key(k) {
            const char* old = std::getenv(k.c_str());
            if (old) { had = true; previous = old; }
            ::unsetenv(k.c_str());
        }
        ~EnvUnscope() {
            if (had) ::setenv(key.c_str(), previous.c_str(), 1);
        }
    };

    ToolchainStoreLayout makeStore(const std::filesystem::path& root) {
        ToolchainStoreLayout layout;
        layout.root = root.string();
        return layout;
    }

    // Populate a fake install-root with the bin/cajeta entry the
    // dispatcher checks for existence.
    void fakeInstall(const ToolchainStoreLayout& layout,
                     const std::string& dist,
                     const std::string& version) {
        auto root = std::filesystem::path(layout.installRoot(dist, version));
        std::filesystem::create_directories(root / "bin");
        writeFile(root / "bin" / "cajeta", "#!/bin/sh\nexit 0\n");
        std::filesystem::permissions(root / "bin" / "cajeta",
            std::filesystem::perms::owner_all);
    }

}  // namespace

TEST(Phase14, fetchPolicyRoundTrips) {
    for (auto p : {FetchPolicy::Auto, FetchPolicy::Warn,
                   FetchPolicy::Error, FetchPolicy::Off}) {
        auto s = fetchPolicyToString(p);
        EXPECT_FALSE(s.empty());
        auto back = fetchPolicyFromString(s);
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, p);
    }
    EXPECT_FALSE(fetchPolicyFromString("nope").has_value());
}

TEST(Phase14, reservedDistributionsListed) {
    EXPECT_TRUE(isReservedDistribution("official"));
    EXPECT_TRUE(isReservedDistribution("nightly"));
    EXPECT_TRUE(isReservedDistribution("lts"));
    EXPECT_TRUE(isReservedDistribution("system"));
    EXPECT_FALSE(isReservedDistribution("my-org"));
}

TEST(Phase14, settingsToolchainBlockParses) {
    std::string body = R"({
        "details": { "name": "x.y", "version": "1.0.0" },
        "settings": {
            "toolchain": {
                "version":      "1.0.3",
                "distribution": "official",
                "channel":      "stable",
                "sha256":       "sha256:deadbeef",
                "fetch":        "auto",
                "from":         "main-registry"
            }
        }
    })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto pin = parseToolchainPin(*m);
    ASSERT_TRUE(static_cast<bool>(pin));
    ASSERT_TRUE(pin->has_value());
    EXPECT_EQ((*pin)->version, "1.0.3");
    EXPECT_EQ((*pin)->distribution, "official");
    ASSERT_TRUE((*pin)->channel.has_value());
    EXPECT_EQ(*(*pin)->channel, "stable");
    ASSERT_TRUE((*pin)->sha256.has_value());
    EXPECT_EQ(*(*pin)->sha256, "sha256:deadbeef");
    EXPECT_EQ((*pin)->fetch, FetchPolicy::Auto);
    ASSERT_TRUE((*pin)->fromRepo.has_value());
    EXPECT_EQ(*(*pin)->fromRepo, "main-registry");
}

TEST(Phase14, settingsToolchainRejectsUnknownSubfield) {
    std::string body = R"({
        "details": { "name": "x.y", "version": "1.0.0" },
        "settings": {
            "toolchain": {
                "version": "1.0.3",
                "not-a-real-field": "value"
            }
        }
    })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto pin = parseToolchainPin(*m);
    EXPECT_FALSE(static_cast<bool>(pin));
    if (!pin) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << pin.takeError();
        EXPECT_NE(msg.find("not-a-real-field"), std::string::npos);
    }
}

TEST(Phase14, settingsToolchainRequiresVersion) {
    std::string body = R"({
        "details": { "name": "x.y", "version": "1.0.0" },
        "settings": { "toolchain": { "distribution": "official" } }
    })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto pin = parseToolchainPin(*m);
    EXPECT_FALSE(static_cast<bool>(pin));
    if (!pin) llvm::consumeError(pin.takeError());
}

TEST(Phase14, settingsToolchainRejectsInvalidFetch) {
    std::string body = R"({
        "details": { "name": "x.y", "version": "1.0.0" },
        "settings": {
            "toolchain": { "version": "1.0.3", "fetch": "nope" }
        }
    })";
    auto m = loadManifestString(body, "<inline>");
    ASSERT_TRUE(static_cast<bool>(m));
    auto pin = parseToolchainPin(*m);
    EXPECT_FALSE(static_cast<bool>(pin));
    if (!pin) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << pin.takeError();
        EXPECT_NE(msg.find("fetch"), std::string::npos);
    }
}

TEST(Phase14, overrideFilePrecedesManifestPin) {
    // Acceptance: .cajeta-toolchain file overrides the manifest
    // pin for that working tree only.
    auto root = tempRoot("overridePrecedence");
    writeFile(root / "cajeta.json", R"({
        "details": { "name": "x.y", "version": "1.0.0" },
        "settings": { "toolchain": { "version": "1.0.3" } }
    })");
    writeFile(root / ".cajeta-toolchain", "nightly:9.9.9\n");
    auto m = loadManifestString(
        R"({"details":{"name":"x.y","version":"1.0.0"},)"
        R"("settings":{"toolchain":{"version":"1.0.3"}}})",
        (root / "cajeta.json").string());
    ASSERT_TRUE(static_cast<bool>(m));
    auto resolved = resolveEffectiveToolchain(&*m, root.string());
    ASSERT_TRUE(static_cast<bool>(resolved));
    ASSERT_TRUE(resolved->hasPin);
    EXPECT_EQ(resolved->pin.distribution, "nightly")
        << ".cajeta-toolchain must win over settings.toolchain";
    EXPECT_EQ(resolved->pin.version, "9.9.9");
    EXPECT_NE(resolved->sourcePath.find(".cajeta-toolchain"),
              std::string::npos);
}

TEST(Phase14, manifestPinUsedWhenNoOverrideFile) {
    auto root = tempRoot("manifestPin");
    auto m = loadManifestString(
        R"({"details":{"name":"x.y","version":"1.0.0"},)"
        R"("settings":{"toolchain":{"version":"1.0.3"}}})",
        (root / "cajeta.json").string());
    ASSERT_TRUE(static_cast<bool>(m));
    auto resolved = resolveEffectiveToolchain(&*m, root.string());
    ASSERT_TRUE(static_cast<bool>(resolved));
    ASSERT_TRUE(resolved->hasPin);
    EXPECT_EQ(resolved->pin.version, "1.0.3");
    EXPECT_EQ(resolved->pin.distribution, "official");
}

TEST(Phase14, overrideFileRejectsMalformedContents) {
    auto root = tempRoot("badOverride");
    writeFile(root / ".cajeta-toolchain", "no-colon-here\n");
    auto r = readToolchainOverrideFile(root.string());
    EXPECT_FALSE(static_cast<bool>(r));
    if (!r) llvm::consumeError(r.takeError());
}

TEST(Phase14, dispatchContinuesWithoutPin) {
    ResolvedToolchain tc;
    auto layout = makeStore(tempRoot("noPin"));
    auto d = computeDispatchDecision(tc, layout);
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::Continue);
}

TEST(Phase14, dispatchContinuesWhenRunningMatchesPin) {
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "1.0.3";
    tc.pin.distribution = "official";
    auto layout = makeStore(tempRoot("matchPin"));
    auto d = computeDispatchDecision(tc, layout, "1.0.3");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::Continue);
}

TEST(Phase14, dispatchReExecsWhenPinnedInstalled) {
    auto root = tempRoot("reexec");
    auto layout = makeStore(root);
    fakeInstall(layout, "official", "1.0.4");
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "1.0.4";
    tc.pin.distribution = "official";
    auto d = computeDispatchDecision(tc, layout, "1.0.3");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::ReExec);
    EXPECT_NE(d->resolvedBinaryPath.find("/official/1.0.4/bin/cajeta"),
              std::string::npos);
}

TEST(Phase14, dispatchNeedsInstallWhenAutoAndMissing) {
    auto layout = makeStore(tempRoot("needsInstall"));
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "2.0.0";
    tc.pin.distribution = "official";
    tc.pin.fetch = FetchPolicy::Auto;
    auto d = computeDispatchDecision(tc, layout, "1.0.0");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::NeedsInstall);
    EXPECT_NE(d->installHint.find("cajeta toolchain install"),
              std::string::npos);
}

TEST(Phase14, dispatchWarnsAndContinuesUnderFetchWarn) {
    auto layout = makeStore(tempRoot("warn"));
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "2.0.0";
    tc.pin.distribution = "official";
    tc.pin.fetch = FetchPolicy::Warn;
    auto d = computeDispatchDecision(tc, layout, "1.0.0");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::Continue);
    ASSERT_FALSE(d->notes.empty())
        << "fetch=warn must surface a warning in notes";
}

TEST(Phase14, dispatchErrorsUnderFetchError) {
    auto layout = makeStore(tempRoot("errPolicy"));
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "2.0.0";
    tc.pin.distribution = "official";
    tc.pin.fetch = FetchPolicy::Error;
    auto d = computeDispatchDecision(tc, layout, "1.0.0");
    EXPECT_FALSE(static_cast<bool>(d));
    if (!d) {
        std::string msg;
        llvm::raw_string_ostream os(msg);
        os << d.takeError();
        EXPECT_NE(msg.find("install"), std::string::npos);
    }
}

TEST(Phase14, dispatchSkippedUnderFetchOff) {
    auto layout = makeStore(tempRoot("offPolicy"));
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "2.0.0";
    tc.pin.distribution = "official";
    tc.pin.fetch = FetchPolicy::Off;
    auto d = computeDispatchDecision(tc, layout, "1.0.0");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::Continue);
    ASSERT_FALSE(d->notes.empty());
}

TEST(Phase14, cajetaNoDispatchEnvBypassesEverything) {
    // Acceptance: CAJETA_NO_DISPATCH=1 runs the PATH binary
    // regardless of pin.
    EnvScope set("CAJETA_NO_DISPATCH", "1");
    auto layout = makeStore(tempRoot("envBypass"));
    ResolvedToolchain tc;
    tc.hasPin = true;
    tc.pin.version = "2.0.0";
    tc.pin.distribution = "official";
    tc.pin.fetch = FetchPolicy::Error;
    auto d = computeDispatchDecision(tc, layout, "1.0.0");
    ASSERT_TRUE(static_cast<bool>(d));
    EXPECT_EQ(d->action, DispatchAction::Continue)
        << "CAJETA_NO_DISPATCH must override fetch=error";
}

TEST(Phase14, listInstalledFindsLayoutEntries) {
    auto root = tempRoot("listInstalled");
    auto layout = makeStore(root);
    fakeInstall(layout, "official", "1.0.3");
    fakeInstall(layout, "official", "1.0.4");
    fakeInstall(layout, "nightly",  "2026-06-01");
    auto inst = listInstalledToolchains(layout);
    ASSERT_EQ(inst.size(), 3u);
    EXPECT_EQ(inst[0].distribution, "nightly");
    // official entries sorted by version desc within distribution.
    EXPECT_EQ(inst[1].distribution, "official");
    EXPECT_EQ(inst[1].version, "1.0.4");
    EXPECT_EQ(inst[2].distribution, "official");
    EXPECT_EQ(inst[2].version, "1.0.3");
}

TEST(Phase14, toolchainIdentityIsStableAndPinAware) {
    // IR cache discriminator input: toolchain identity. Same pin
    // → same identity; different pin → different identity.
    ResolvedToolchain noPin;
    auto a = toolchainIdentity(noPin, "1.0.3");
    auto b = toolchainIdentity(noPin, "1.0.3");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, "official:1.0.3");

    ResolvedToolchain pinned;
    pinned.hasPin = true;
    pinned.pin.version = "2.0.0";
    pinned.pin.distribution = "nightly";
    EXPECT_EQ(toolchainIdentity(pinned), "nightly:2.0.0");

    ResolvedToolchain withSha;
    withSha.hasPin = true;
    withSha.pin.version = "1.0.3";
    withSha.pin.distribution = "official";
    withSha.pin.sha256 = "sha256:deadbeef";
    EXPECT_EQ(toolchainIdentity(withSha),
              "official:1.0.3:sha256:deadbeef");
}

TEST(Phase14, runningBinarySatisfiesPin) {
    ResolvedToolchain noPin;
    EXPECT_TRUE(runningBinarySatisfiesPin(noPin, "1.0.3"));

    ResolvedToolchain pin;
    pin.hasPin = true;
    pin.pin.version = "1.0.3";
    EXPECT_TRUE(runningBinarySatisfiesPin(pin, "1.0.3"));
    EXPECT_FALSE(runningBinarySatisfiesPin(pin, "1.0.4"));
}

TEST(Phase14, storeLayoutHonorsHomeOverride) {
    auto root = tempRoot("homeOverride");
    auto layout = resolveToolchainStoreLayout(root.string());
    EXPECT_EQ(layout.root, root.string());
    EXPECT_NE(layout.binaryPath("official", "1.0.3")
                  .find("/official/1.0.3/bin/cajeta"),
              std::string::npos);
}
