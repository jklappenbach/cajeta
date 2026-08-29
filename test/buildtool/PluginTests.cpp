// Regression tests for the Phase 7b plugin layer:
//   - parsePlugins reads the `plugins` block.
//   - parsePluginsAllowedCapabilities reads
//     `settings.plugins-allowed-capabilities`.
//   - resolvePlugins fetches each plugin through the repo machinery
//     and validates its declared capabilities against the allowlist.
//   - First-party vs user-plugin capability defaults.
//   - Lockfile records typed plugin entries with checksum + caps.

#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Plugin.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::ArtifactCache;
using cajeta::buildtool::composeLockfileWithResolution;
using cajeta::buildtool::defaultFirstPartyPluginAllowlist;
using cajeta::buildtool::defaultUserPluginAllowlist;
using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::isFirstPartyPluginName;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parsePlugins;
using cajeta::buildtool::parsePluginsAllowedCapabilities;
using cajeta::buildtool::PluginSpec;
using cajeta::buildtool::readLockfile;
using cajeta::buildtool::RepositoryPtr;
using cajeta::buildtool::ResolvedPlugin;
using cajeta::buildtool::resolvePlugins;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::writeLockfile;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-plugin-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Stage a "plugin" package in the filesystem repo: writes a
    // `.cja` stub + sidecar manifest with the given capabilities.
    void stagePlugin(const std::filesystem::path& root,
                     const std::string& name,
                     const std::string& version,
                     const std::vector<std::string>& caps) {
        auto dir = root / name / version;
        std::filesystem::create_directories(dir);
        { std::ofstream o(dir / (name + "-" + version + ".cja"),
                          std::ios::binary);
          o << "stub"; }
        std::ostringstream js;
        js << "{\"details\":{\"name\":\"" << name
           << "\",\"version\":\"" << version << "\"}";
        if (!caps.empty()) {
            js << ",\"settings\":{\"capabilities\":[";
            for (size_t i = 0; i < caps.size(); ++i) {
                if (i) js << ",";
                js << "\"" << caps[i] << "\"";
            }
            js << "]}";
        }
        js << "}";
        std::ofstream o(dir / "cajeta.json", std::ios::binary);
        o << js.str();
    }

} // namespace

// ─── parsePlugins ─────────────────────────────────────────────────

TEST(PluginTests, parsePluginsReadsBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "0.1.0" },
        "plugins": {
            "cajeta.coverage": {
                "version": "1.0.*",
                "config": { "min": 80, "grain": "line" }
            },
            "acme.policy-gate": { "version": "2.1.0" }
        }
    })");
    auto specs = parsePlugins(m);
    ASSERT_TRUE((bool)specs) << errorText(specs.takeError());
    ASSERT_EQ(specs->size(), 2u);

    // Iteration order on llvm::json::Object isn't insertion-order,
    // so look up by name.
    PluginSpec cov, gate;
    for (const auto& p : *specs) {
        if (p.name == "cajeta.coverage")    cov  = p;
        else if (p.name == "acme.policy-gate") gate = p;
    }
    EXPECT_EQ(cov.versionConstraint, "1.0.*");
    EXPECT_FALSE(cov.configRaw.empty());
    EXPECT_EQ(gate.versionConstraint, "2.1.0");
    EXPECT_TRUE(gate.configRaw.empty());
}

TEST(PluginTests, parsePluginsErrorsOnMissingVersion) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "0.1.0" },
        "plugins": { "acme.thing": { "config": {} } }
    })");
    auto specs = parsePlugins(m);
    ASSERT_FALSE((bool)specs);
    EXPECT_NE(errorText(specs.takeError()).find("missing required 'version'"),
              std::string::npos);
}

// The string shorthand is the form `cajeta init`'s archetypes SHIP, and it
// mirrors how `dependencies` is spelled (name -> version constraint). It was
// rejected here, so `cajeta init basic` wrote a manifest that could not be
// built: "plugins.cajeta.lint.security: value must be an object with
// 'version'". ManifestEditor.cpp already documented the shorthand as
// something "the plugin resolver itself accepts" and rewrites it in place for
// `cajeta coverage ignore` — that belief was simply not true of parsePlugins,
// which is the gap this closes.
//
// This test replaces `parsePluginsErrorsOnNonObject`, which pinned the
// rejecting behaviour with this exact input.
TEST(PluginTests, parsePluginsAcceptsTheStringShorthand) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "0.1.0" },
        "plugins": { "acme.thing": "1.0.0" }
    })");
    auto specs = parsePlugins(m);
    ASSERT_TRUE((bool)specs) << errorText(specs.takeError());
    ASSERT_EQ(specs->size(), 1u);
    EXPECT_EQ((*specs)[0].name, "acme.thing");
    EXPECT_EQ((*specs)[0].versionConstraint, "1.0.0")
        << "the shorthand's value IS the version constraint";
    EXPECT_TRUE((*specs)[0].configRaw.empty())
        << "the shorthand carries no config";
}

// The exact block `cajeta init basic` writes — the regression that motivated
// the change, asserted against the real archetype spelling rather than a
// reconstruction of it.
TEST(PluginTests, parsePluginsAcceptsWhatTheBasicArchetypeShips) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "0.1.0" },
        "plugins": {
            "cajeta.lint.security": "1.0.*",
            "cajeta.coverage":      "1.0.*"
        }
    })");
    auto specs = parsePlugins(m);
    ASSERT_TRUE((bool)specs) << errorText(specs.takeError());
    EXPECT_EQ(specs->size(), 2u);
}

// The arm that must still FIRE: a value that is neither a string nor an
// object is still an error. Widening the accepted shapes must not turn the
// check off — a validator that accepts everything reports nothing.
TEST(PluginTests, parsePluginsStillErrorsOnAValueThatIsNeither) {
    for (const char* bad : {"42", "[\"1.0.0\"]", "true", "null"}) {
        std::string src = std::string(R"({
            "details": { "name": "p", "version": "0.1.0" },
            "plugins": { "acme.thing": )") + bad + " } }";
        auto m = mustLoad(src);
        auto specs = parsePlugins(m);
        ASSERT_FALSE((bool)specs) << "accepted a bad value: " << bad;
        EXPECT_NE(errorText(specs.takeError()).find("acme.thing"),
                  std::string::npos)
            << "the diagnostic must name the offending plugin, for: " << bad;
    }
}


// ─── parsePluginsAllowedCapabilities ──────────────────────────────

TEST(PluginTests, parseAllowedCapsReadsArray) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "0.1.0" },
        "settings": {
            "plugins-allowed-capabilities": ["filesystem", "network"]
        }
    })");
    auto caps = parsePluginsAllowedCapabilities(m);
    ASSERT_TRUE((bool)caps);
    ASSERT_EQ(caps->size(), 2u);
    EXPECT_EQ((*caps)[0], "filesystem");
    EXPECT_EQ((*caps)[1], "network");
}


// ─── default allowlists + namespace predicate ─────────────────────


TEST(PluginTests, defaultFirstPartyAllowlistIsWider) {
    auto d = defaultFirstPartyPluginAllowlist();
    EXPECT_NE(std::find(d.begin(), d.end(), "filesystem"), d.end());
    EXPECT_NE(std::find(d.begin(), d.end(), "process"),    d.end());
    EXPECT_NE(std::find(d.begin(), d.end(), "network"),    d.end());
}


// ─── resolvePlugins ───────────────────────────────────────────────

TEST(PluginTests, resolvePluginsFetchesHighestSatisfying) {
    auto root = makeTempDir("repo");
    stagePlugin(root, "acme.policy-gate", "1.0.0", {"filesystem"});
    stagePlugin(root, "acme.policy-gate", "1.5.0", {"filesystem"});
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto proj = makeTempDir("proj");
    auto home = makeTempDir("home");
    ArtifactCache cache(proj.string(), home.string());

    std::vector<PluginSpec> specs;
    { PluginSpec p; p.name = "acme.policy-gate";
      p.versionConstraint = "1.*"; specs.push_back(p); }

    auto resolved = resolvePlugins(specs, repos, {"filesystem"}, cache);
    ASSERT_TRUE((bool)resolved) << errorText(resolved.takeError());
    ASSERT_EQ(resolved->size(), 1u);
    EXPECT_EQ((*resolved)[0].name, "acme.policy-gate");
    EXPECT_EQ((*resolved)[0].version, "1.5.0");
    EXPECT_EQ((*resolved)[0].capabilities.size(), 1u);
    EXPECT_TRUE((*resolved)[0].capabilities.count("filesystem") == 1u);
}



TEST(PluginTests, resolvePluginsUserPluginUsesNarrowDefault) {
    auto root = makeTempDir("repo");
    // Non-cajeta plugin requests `process` — should fail with the
    // default user allowlist (filesystem-only) when no explicit
    // allowlist was declared.
    stagePlugin(root, "acme.thing", "1.0.0",
                {"filesystem", "process"});
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto proj = makeTempDir("proj");
    auto home = makeTempDir("home");
    ArtifactCache cache(proj.string(), home.string());

    std::vector<PluginSpec> specs;
    { PluginSpec p; p.name = "acme.thing";
      p.versionConstraint = "1.0.0"; specs.push_back(p); }

    auto resolved = resolvePlugins(specs, repos, {}, cache);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find("'process'"),
              std::string::npos);
}

TEST(PluginTests, resolvePluginsErrorsOnMissingArtifact) {
    auto root = makeTempDir("repo");  // empty
    std::vector<RepositoryPtr> repos = {
        std::make_shared<FilesystemRepository>("test", root.string()),
    };
    auto proj = makeTempDir("proj");
    auto home = makeTempDir("home");
    ArtifactCache cache(proj.string(), home.string());

    std::vector<PluginSpec> specs;
    { PluginSpec p; p.name = "acme.missing";
      p.versionConstraint = "1.0.0"; specs.push_back(p); }

    auto resolved = resolvePlugins(specs, repos, {"filesystem"}, cache);
    ASSERT_FALSE((bool)resolved);
    EXPECT_NE(errorText(resolved.takeError()).find(
                  "no repository has a satisfying version"),
              std::string::npos);
}

// ─── lockfile recording ───────────────────────────────────────────

TEST(PluginTests, lockfileRecordsResolvedPlugins) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto props = resolveProperties(m);
    ASSERT_TRUE((bool)props);

    std::vector<ResolvedPlugin> plugins;
    {
        ResolvedPlugin p;
        p.name = "cajeta.coverage";
        p.version = "1.0.0";
        p.resolvedFromRepo = "central";
        p.sha256 = "sha256:ppp";
        p.capabilities = {"filesystem", "process"};
        plugins.push_back(p);
    }

    cajeta::buildtool::MeltResolution melts;
    std::map<std::string, std::string> providedBy;
    auto lf = composeLockfileWithResolution(
        m, "manifest-source", *props, {}, melts, providedBy,
        plugins, "2026-06-01T00:00:00Z");

    ASSERT_EQ(lf.pluginsTyped.size(), 1u);
    EXPECT_EQ(lf.pluginsTyped[0].name, "cajeta.coverage");
    // Capabilities should be alphabetised on the way out.
    ASSERT_EQ(lf.pluginsTyped[0].capabilities.size(), 2u);
    EXPECT_EQ(lf.pluginsTyped[0].capabilities[0], "filesystem");
    EXPECT_EQ(lf.pluginsTyped[0].capabilities[1], "process");

    // Round-trip through write + read.
    auto path = (makeTempDir("lock") / "cajeta.lock").string();
    ASSERT_FALSE((bool)writeLockfile(path, lf));
    auto loaded = readLockfile(path);
    ASSERT_TRUE((bool)loaded);
    ASSERT_EQ(loaded->pluginsTyped.size(), 1u);
    EXPECT_EQ(loaded->pluginsTyped[0].name, "cajeta.coverage");
    EXPECT_EQ(loaded->pluginsTyped[0].checksum, "sha256:ppp");
    ASSERT_EQ(loaded->pluginsTyped[0].capabilities.size(), 2u);
    EXPECT_EQ(loaded->pluginsTyped[0].capabilities[0], "filesystem");
}
