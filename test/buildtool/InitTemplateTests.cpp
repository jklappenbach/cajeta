// Regression tests for the embedded `cajeta init` archetype
// templates (docs/BuildTool.md "Project shapes"). These
// guard:
//   - that every archetype declared in src/CMakeLists.txt actually
//     embeds (drift between the sample tree and the CMake glob),
//   - that each archetype's embedded cajeta.json parses through
//     loadManifestString (no malformed template ever ships),
//   - that instantiateInitTemplate writes the exact bytes that
//     were embedded (round-trip), and
//   - that the overwrite-refusal contract holds.

#include "cajeta/buildtool/InitTemplates.h"
#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using cajeta::buildtool::availableInitTemplates;
using cajeta::buildtool::findInitTemplate;
using cajeta::buildtool::instantiateInitTemplate;
using cajeta::buildtool::loadManifestString;

namespace {

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto base = std::filesystem::temp_directory_path() /
            ("cajeta-init-test-" + tag + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(&tag)));
        std::filesystem::remove_all(base);
        // Don't pre-create: instantiateInitTemplate should create
        // the destination if it doesn't exist. Tests that need a
        // pre-existing dir create it explicitly.
        return base;
    }

    std::string readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

} // namespace

TEST(InitTemplateTests, embedsAllFiveArchetypes) {
    auto names = availableInitTemplates();
    std::set<std::string> nameSet(names.begin(), names.end());
    EXPECT_EQ(names.size(), 5u);
    EXPECT_TRUE(nameSet.count("basic"));
    EXPECT_TRUE(nameSet.count("library"));
    EXPECT_TRUE(nameSet.count("workspace"));
    EXPECT_TRUE(nameSet.count("multi-binary"));
    EXPECT_TRUE(nameSet.count("melt"));
}

TEST(InitTemplateTests, unknownArchetypeIsNullopt) {
    EXPECT_FALSE(findInitTemplate("does-not-exist").has_value());
}

TEST(InitTemplateTests, basicHasManifestAndOneSource) {
    auto t = findInitTemplate("basic");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->name, "basic");
    // The basic archetype embeds cajeta.json + the Main.cajeta
    // source. README.md and run.sh are excluded by convention.
    EXPECT_EQ(t->files.size(), 2u);

    bool hasManifest = false;
    bool hasSource = false;
    for (const auto& f : t->files) {
        if (f.relativePath == "cajeta.json") hasManifest = true;
        if (f.relativePath.find("Main.cajeta") != std::string::npos) {
            hasSource = true;
        }
        // No sample-meta files leaked through.
        EXPECT_EQ(f.relativePath.find("README"), std::string::npos);
        EXPECT_EQ(f.relativePath.find("run.sh"), std::string::npos);
    }
    EXPECT_TRUE(hasManifest);
    EXPECT_TRUE(hasSource);
}

TEST(InitTemplateTests, meltIsManifestOnly) {
    auto t = findInitTemplate("melt");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->files.size(), 1u);
    EXPECT_EQ(t->files[0].relativePath, "cajeta.json");
}

TEST(InitTemplateTests, workspaceHasMultipleManifests) {
    auto t = findInitTemplate("workspace");
    ASSERT_TRUE(t.has_value());
    // Workspace root + 4 members (shared/core, shared/util,
    // apps/api, apps/cli) = 5 manifests, plus per-member sources.
    int manifestCount = 0;
    for (const auto& f : t->files) {
        if (f.relativePath.size() >= 12 &&
            f.relativePath.compare(
                f.relativePath.size() - 12, 12, "/cajeta.json") == 0) {
            ++manifestCount;
        }
        if (f.relativePath == "cajeta.json") ++manifestCount;
    }
    EXPECT_EQ(manifestCount, 5);
}

TEST(InitTemplateTests, everyManifestParses) {
    for (const auto& name : availableInitTemplates()) {
        auto t = findInitTemplate(name);
        ASSERT_TRUE(t.has_value()) << name;
        for (const auto& f : t->files) {
            // Manifest files end in `cajeta.json`. Parse each
            // through the real loader so a template with a
            // malformed manifest fails the build (catches drift
            // between samples and the manifest validator).
            if (f.relativePath.size() >= 11 &&
                f.relativePath.compare(
                    f.relativePath.size() - 11, 11, "cajeta.json") == 0) {
                auto m = loadManifestString(
                    std::string(f.contents),
                    name + ":" + f.relativePath);
                if (!m) {
                    std::string err;
                    llvm::raw_string_ostream os(err);
                    os << m.takeError();
                    FAIL() << "template '" << name << "' manifest '"
                           << f.relativePath << "' failed to parse: " << err;
                }
            }
        }
    }
}

TEST(InitTemplateTests, instantiateBasicRoundTripsBytes) {
    auto dest = makeTempDir("basic-rt");
    auto result = instantiateInitTemplate("basic", dest.string(), false);
    ASSERT_TRUE(static_cast<bool>(result))
        << "instantiate failed: " << llvm::toString(result.takeError());

    auto t = findInitTemplate("basic");
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(result->filesWritten.size(), t->files.size());

    for (const auto& f : t->files) {
        auto path = dest / f.relativePath;
        ASSERT_TRUE(std::filesystem::exists(path)) << path;
        std::string disk = readFile(path);
        EXPECT_EQ(disk, std::string(f.contents))
            << "byte-mismatch on " << path;
    }
    std::filesystem::remove_all(dest);
}

TEST(InitTemplateTests, instantiateCreatesNestedDirectories) {
    auto dest = makeTempDir("nested");
    auto result = instantiateInitTemplate(
        "multi-binary", dest.string(), false);
    ASSERT_TRUE(static_cast<bool>(result));

    // multi-binary has src/main/cajeta/com/example/multi/<bin>/Main.cajeta
    // — verify that deep path was created.
    bool foundNested = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dest)) {
        if (entry.is_regular_file()) {
            auto s = entry.path().string();
            if (s.find("server") != std::string::npos &&
                s.find("Main.cajeta") != std::string::npos) {
                foundNested = true;
            }
        }
    }
    EXPECT_TRUE(foundNested);
    std::filesystem::remove_all(dest);
}

TEST(InitTemplateTests, refusesToOverwriteWithoutForce) {
    auto dest = makeTempDir("collide");
    std::filesystem::create_directories(dest);
    // Pre-create cajeta.json with sentinel content.
    std::ofstream(dest / "cajeta.json") << "{ \"already-here\": true }";

    auto result = instantiateInitTemplate("basic", dest.string(), false);
    EXPECT_FALSE(static_cast<bool>(result));
    if (!result) {
        std::string msg = llvm::toString(result.takeError());
        EXPECT_NE(msg.find("would overwrite"), std::string::npos)
            << "expected overwrite-refusal message, got: " << msg;
    }

    // The pre-existing file is untouched (pre-flight aborted before
    // any write happened).
    EXPECT_EQ(readFile(dest / "cajeta.json"),
              "{ \"already-here\": true }");
    std::filesystem::remove_all(dest);
}

TEST(InitTemplateTests, forceOverwrites) {
    auto dest = makeTempDir("force");
    std::filesystem::create_directories(dest);
    std::ofstream(dest / "cajeta.json") << "stale";

    auto result = instantiateInitTemplate("basic", dest.string(), true);
    ASSERT_TRUE(static_cast<bool>(result));

    auto t = findInitTemplate("basic");
    ASSERT_TRUE(t.has_value());
    // Find the embedded cajeta.json content and verify on-disk
    // matches it (not "stale" anymore).
    for (const auto& f : t->files) {
        if (f.relativePath == "cajeta.json") {
            EXPECT_EQ(readFile(dest / "cajeta.json"),
                      std::string(f.contents));
        }
    }
    std::filesystem::remove_all(dest);
}

TEST(InitTemplateTests, errorsOnUnknownTemplate) {
    auto dest = makeTempDir("unknown");
    auto result = instantiateInitTemplate("does-not-exist",
                                          dest.string(), false);
    EXPECT_FALSE(static_cast<bool>(result));
    if (!result) {
        std::string msg = llvm::toString(result.takeError());
        EXPECT_NE(msg.find("unknown template"), std::string::npos);
        EXPECT_NE(msg.find("basic"), std::string::npos)
            << "error should list available templates; got: " << msg;
    }
}
