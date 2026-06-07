// Regression tests for the build-tool manifest loader.
// See src/cajeta/buildtool/Manifest.h and
// plans/buildtool/build-tool-plan.md Phase 0.

#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
using cajeta::buildtool::ManifestDetails;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

} // namespace

// --- happy path ----------------------------------------------------------

TEST(ManifestTests, loadsMinimalValidManifest) {
    std::string src = R"({
        "details": {
            "name":    "com.example.foo",
            "version": "0.1.0"
        }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool)m);
    EXPECT_EQ(m->details.name, "com.example.foo");
    EXPECT_EQ(m->details.version, "0.1.0");
    EXPECT_EQ(m->details.group(), "com.example");
    EXPECT_EQ(m->details.library(), "foo");
}

TEST(ManifestTests, loadsFullDetailsBlock) {
    std::string src = R"({
        "details": {
            "name":               "com.example.foo",
            "version":            "1.2.3",
            "description":        "Example",
            "license":            "Apache-2.0",
            "authors":            ["Alice", "Bob"],
            "repository-url":     "https://github.com/example/foo",
            "cajeta-lang-version": "1.0"
        }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool)m);
    EXPECT_EQ(m->details.description.value_or(""), "Example");
    EXPECT_EQ(m->details.license.value_or(""), "Apache-2.0");
    EXPECT_EQ(m->details.authors.size(), 2u);
    EXPECT_EQ(m->details.authors[0], "Alice");
    EXPECT_EQ(m->details.repositoryUrl.value_or(""),
              "https://github.com/example/foo");
    EXPECT_EQ(m->details.cajetaLangVersion.value_or(""), "1.0");
}

TEST(ManifestTests, loadsAllSixTopLevelBlocks) {
    std::string src = R"({
        "details":    { "name": "a.b", "version": "0.1" },
        "properties": { "v": "1.0" },
        "settings":   { "capabilities": ["network"] },
        "actions":    { "ship": { "extends": "upload" } },
        "plugins":    { "cajeta.coverage": { "version": "1.0.*" } },
        "tasks":      { "build": { "actions": [] } }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool)m);
    EXPECT_EQ(m->propertiesRaw.size(), 1u);
    EXPECT_EQ(m->settingsRaw.size(), 1u);
    EXPECT_EQ(m->actionsRaw.size(), 1u);
    EXPECT_EQ(m->pluginsRaw.size(), 1u);
    EXPECT_EQ(m->tasksRaw.size(), 1u);
}

TEST(ManifestTests, groupAndLibraryDeriveFromReverseDnsName) {
    auto checkSplit = [](const std::string& name,
                         const std::string& expectedGroup,
                         const std::string& expectedLib) {
        std::string src =
            R"({ "details": { "name": ")" + name +
            R"(", "version": "0.1" } })";
        auto m = loadManifestString(src);
        ASSERT_TRUE((bool)m);
        EXPECT_EQ(m->details.group(), expectedGroup);
        EXPECT_EQ(m->details.library(), expectedLib);
    };
    checkSplit("com.example.foo", "com.example", "foo");
    checkSplit("cajeta.io.net.http", "cajeta.io.net", "http");
    checkSplit("monoartifact", "", "monoartifact");
}

// --- error cases (each documented validation has a regression test) ------

TEST(ManifestTests, errorsOnMissingDetailsBlock) {
    auto m = loadManifestString(R"({"properties":{}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("missing required top-level block 'details'"),
              std::string::npos);
}

TEST(ManifestTests, errorsOnMissingDetailsName) {
    auto m = loadManifestString(R"({"details":{"version":"0.1"}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("missing required field 'name'"), std::string::npos);
}

TEST(ManifestTests, errorsOnMissingDetailsVersion) {
    auto m = loadManifestString(R"({"details":{"name":"a.b"}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("missing required field 'version'"), std::string::npos);
}

TEST(ManifestTests, errorsOnNonStringDetailsName) {
    auto m = loadManifestString(R"({"details":{"name":42,"version":"0.1"}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'name' must be a string"), std::string::npos);
}

TEST(ManifestTests, errorsOnUnknownTopLevelBlock) {
    // Typo: "settigns" instead of "settings".
    auto m = loadManifestString(
        R"({"details":{"name":"a.b","version":"0.1"},"settigns":{}})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("unknown top-level block 'settigns'"),
              std::string::npos);
    // The error lists the valid block names so the developer can find
    // the typo without consulting the docs.
    EXPECT_NE(msg.find("settings"), std::string::npos);
}

TEST(ManifestTests, errorsOnUnknownDetailsField) {
    // Strict schema for `details`: anything unrecognized errors.
    auto m = loadManifestString(R"({
        "details": {
            "name":    "a.b",
            "version": "0.1",
            "bogus":   "field"
        }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("unknown field in 'details': 'bogus'"),
              std::string::npos);
}

TEST(ManifestTests, errorsWhenBlockIsNotAnObject) {
    // `settings` as a string instead of an object.
    auto m = loadManifestString(
        R"({"details":{"name":"a.b","version":"0.1"},"settings":"oops"})");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'settings' must be an object"), std::string::npos);
}

TEST(ManifestTests, errorsOnAuthorsContainingNonString) {
    auto m = loadManifestString(R"({
        "details": {
            "name":    "a.b",
            "version": "0.1",
            "authors": ["Alice", 42]
        }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'authors[1]' must be a string"), std::string::npos);
}

TEST(ManifestTests, errorsOnMalformedJson) {
    auto m = loadManifestString("{ not json");
    ASSERT_FALSE((bool)m);
    // Just confirm the error mentions where it came from.
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("<inline>"), std::string::npos);
}

TEST(ManifestTests, errorsOnRootBeingNonObject) {
    auto m = loadManifestString("[1, 2, 3]");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("manifest root must be a JSON object"),
              std::string::npos);
}

// ─── Melt mutual exclusion (Phase 6c acceptance) ──────────────────

TEST(ManifestTests, errorsWhenMeltDeclaredAlongsideTasks) {
    auto m = loadManifestString(R"({
        "details": { "name": "p.melt", "version": "1.0.0" },
        "melt": { "dependencies": {} },
        "tasks": { "build": { "actions": [] } }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'melt' and 'tasks'"), std::string::npos);
}

TEST(ManifestTests, errorsWhenMeltDeclaredAlongsideWorkspace) {
    auto m = loadManifestString(R"({
        "details": { "name": "p.melt", "version": "1.0.0" },
        "melt": { "dependencies": {} },
        "workspace": { "members": [] }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("'melt' and 'workspace'"), std::string::npos);
}

TEST(ManifestTests, meltAloneLoadsSuccessfully) {
    // Sanity: melt without tasks/workspace is the canonical melt
    // package shape and must parse cleanly.
    auto m = loadManifestString(R"({
        "details": { "name": "p.melt", "version": "1.0.0" },
        "melt": { "dependencies": {} }
    })");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());
    EXPECT_TRUE(m->hasMelt);
}
