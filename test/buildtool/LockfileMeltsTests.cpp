// Regression tests for the Phase 6c lockfile extensions:
//   - Typed melts[] entries are written + read back.
//   - Typed packages[] entries carry the `provided-by` field
//     ("explicit" for direct pins, "<melt-name>@<melt-version>"
//     for melt-supplied versions).
//   - composeLockfileWithResolution wires the resolver outputs
//     into the lockfile.

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Plugin.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::composeLockfileWithResolution;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Lockfile;
using cajeta::buildtool::MeltImport;
using cajeta::buildtool::MeltResolution;
using cajeta::buildtool::PropertyOverrides;
using cajeta::buildtool::readLockfile;
using cajeta::buildtool::ResolvedDependency;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::writeLockfile;

namespace {

    std::filesystem::path tempPath(const std::string& tag) {
        return std::filesystem::temp_directory_path() /
               ("cajeta-lockmelts-" + tag + "-" +
                std::to_string(::getpid()) + "-" +
                std::to_string(::rand()) + ".lock");
    }

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << "manifest load failed";
            return {};
        }
        return std::move(*m);
    }

    std::string readFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

    cajeta::buildtool::ResolvedProperties resolve(
        const cajeta::buildtool::Manifest& m) {
        PropertyOverrides ov;
        auto p = resolveProperties(m, ov);
        if (!p) {
            ADD_FAILURE() << "property resolution failed";
            return {};
        }
        return std::move(*p);
    }

} // namespace

TEST(LockfileMeltsTests, composeWithResolutionRecordsMeltsAndProvidedBy) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto props = resolve(m);

    std::vector<ResolvedDependency> deps;
    {
        ResolvedDependency d;
        d.name = "acme.lib"; d.version = "1.2.5";
        d.resolvedFromRepo = "central";
        d.sha256 = "sha256:aaa";
        deps.push_back(d);
    }
    {
        ResolvedDependency d;
        d.name = "vendor.special"; d.version = "3.2.1";
        d.resolvedFromRepo = "central";
        d.sha256 = "sha256:bbb";
        deps.push_back(d);
    }

    MeltResolution melts;
    {
        MeltResolution::Resolved r;
        r.name = "platform.melt"; r.version = "1.0.0";
        r.resolvedFromRepo = "central";
        r.sha256 = "sha256:mmm";
        r.transitiveMelts.push_back(
            MeltImport{"inner.melt", "0.1.0"});
        melts.resolvedMelts.push_back(r);
    }

    std::map<std::string, std::string> providedBy{
        {"acme.lib", "platform.melt@1.0.0"},
    };

    auto lf = composeLockfileWithResolution(
        m, "manifest-source", props, deps, melts, providedBy, {},
        "2026-06-01T00:00:00Z");

    ASSERT_EQ(lf.packagesTyped.size(), 2u);
    EXPECT_EQ(lf.packagesTyped[0].name, "acme.lib");
    EXPECT_EQ(lf.packagesTyped[0].providedBy, "platform.melt@1.0.0");
    EXPECT_EQ(lf.packagesTyped[1].name, "vendor.special");
    EXPECT_EQ(lf.packagesTyped[1].providedBy, "explicit");

    ASSERT_EQ(lf.meltsTyped.size(), 1u);
    EXPECT_EQ(lf.meltsTyped[0].name, "platform.melt");
    EXPECT_EQ(lf.meltsTyped[0].version, "1.0.0");
    ASSERT_EQ(lf.meltsTyped[0].transitiveMelts.size(), 1u);
    EXPECT_EQ(lf.meltsTyped[0].transitiveMelts[0], "inner.melt@0.1.0");
}

TEST(LockfileMeltsTests, writeReadRoundtripPreservesTypedSlots) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto props = resolve(m);

    std::vector<ResolvedDependency> deps;
    {
        ResolvedDependency d;
        d.name = "acme.lib"; d.version = "1.2.5";
        d.resolvedFromRepo = "central";
        d.sha256 = "sha256:aaa";
        deps.push_back(d);
    }
    MeltResolution melts;
    {
        MeltResolution::Resolved r;
        r.name = "platform.melt"; r.version = "1.0.0";
        r.resolvedFromRepo = "central";
        r.sha256 = "sha256:mmm";
        melts.resolvedMelts.push_back(r);
    }
    std::map<std::string, std::string> providedBy{
        {"acme.lib", "platform.melt@1.0.0"},
    };

    auto lf = composeLockfileWithResolution(
        m, "manifest-source", props, deps, melts, providedBy, {},
        "2026-06-01T00:00:00Z");

    auto path = tempPath("rt").string();
    ASSERT_FALSE((bool)writeLockfile(path, lf));

    auto loaded = readLockfile(path);
    ASSERT_TRUE((bool)loaded);
    EXPECT_EQ(loaded->packagesTyped.size(), 1u);
    EXPECT_EQ(loaded->packagesTyped[0].name, "acme.lib");
    EXPECT_EQ(loaded->packagesTyped[0].providedBy,
              "platform.melt@1.0.0");
    EXPECT_EQ(loaded->meltsTyped.size(), 1u);
    EXPECT_EQ(loaded->meltsTyped[0].name, "platform.melt");
    EXPECT_EQ(loaded->meltsTyped[0].checksum, "sha256:mmm");

    std::filesystem::remove(path);
}

TEST(LockfileMeltsTests, onDiskShapeIsHumanReadable) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto props = resolve(m);

    std::vector<ResolvedDependency> deps;
    {
        ResolvedDependency d;
        d.name = "acme.lib"; d.version = "1.2.5";
        d.resolvedFromRepo = "central";
        d.sha256 = "sha256:aaa";
        deps.push_back(d);
    }
    MeltResolution melts;
    {
        MeltResolution::Resolved r;
        r.name = "platform.melt"; r.version = "1.0.0";
        r.resolvedFromRepo = "central";
        r.sha256 = "sha256:mmm";
        melts.resolvedMelts.push_back(r);
    }
    std::map<std::string, std::string> providedBy{
        {"acme.lib", "platform.melt@1.0.0"},
    };

    auto lf = composeLockfileWithResolution(
        m, "manifest-source", props, deps, melts, providedBy, {},
        "2026-06-01T00:00:00Z");

    auto path = tempPath("disk").string();
    ASSERT_FALSE((bool)writeLockfile(path, lf));

    std::string contents = readFile(path);
    EXPECT_NE(contents.find("\"melts\""), std::string::npos);
    EXPECT_NE(contents.find("\"platform.melt\""), std::string::npos);
    EXPECT_NE(contents.find("\"provided-by\""), std::string::npos);
    EXPECT_NE(contents.find("\"transitive-melts\""), std::string::npos);

    std::filesystem::remove(path);
}

TEST(LockfileMeltsTests, packagesAndMeltsEmptyWhenNoResolution) {
    auto m = mustLoad(R"({
        "details": { "name": "c", "version": "0.1.0" }
    })");
    auto props = resolve(m);

    MeltResolution melts;
    std::map<std::string, std::string> providedBy;
    auto lf = composeLockfileWithResolution(
        m, "manifest-source", props, {}, melts, providedBy, {},
        "2026-06-01T00:00:00Z");
    EXPECT_TRUE(lf.packagesTyped.empty());
    EXPECT_TRUE(lf.meltsTyped.empty());

    auto path = tempPath("empty").string();
    ASSERT_FALSE((bool)writeLockfile(path, lf));
    std::string contents = readFile(path);
    // Both arrays present and empty so the schema slot is on disk.
    EXPECT_NE(contents.find("\"melts\": []"), std::string::npos);
    EXPECT_NE(contents.find("\"packages\": []"), std::string::npos);
    std::filesystem::remove(path);
}
