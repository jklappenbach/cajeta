// Regression tests for the Phase 6c melt parser:
//   - Manifest-level: `melt` block parses through, and mutual-
//     exclusion with `tasks` / `workspace` is enforced.
//   - parseMelt: each exportable field maps to the typed model;
//     non-exportable fields are rejected.
//   - parseSettingsMelts: `settings.melts[]` parses `name@version`
//     pins and rejects malformed entries.
//   - parseMeltImport: the standalone helper used by CLI flag
//     parsing.

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Melt.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Manifest;
using cajeta::buildtool::Melt;
using cajeta::buildtool::MeltImport;
using cajeta::buildtool::parseMelt;
using cajeta::buildtool::parseMeltImport;
using cajeta::buildtool::parseSettingsMelts;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

} // namespace

// ─── manifest-level: melt block is recognized + preserved ─────────

TEST(MeltParserTests, manifestRetainsMeltBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": {
            "dependencies": { "x.y": "1.0.0" }
        }
    })");
    EXPECT_TRUE(m.hasMelt);
    EXPECT_FALSE(m.meltRaw.empty());
}

TEST(MeltParserTests, manifestWithoutMeltLeavesFlagFalse) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" }
    })");
    EXPECT_FALSE(m.hasMelt);
    EXPECT_TRUE(m.meltRaw.empty());
}

// ─── mutual exclusion ─────────────────────────────────────────────

TEST(MeltParserTests, meltWithTasksIsRejected) {
    auto m = loadManifestString(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": { "dependencies": {} },
        "tasks": { "build": { "actions": [{ "action": "exec",
                                            "params": { "command": "echo" } }] } }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("both 'melt' and 'tasks'"), std::string::npos);
}

TEST(MeltParserTests, meltWithWorkspaceIsRejected) {
    auto m = loadManifestString(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": { "dependencies": {} },
        "workspace": { "members": [] }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("both 'melt' and 'workspace'"), std::string::npos);
}

// ─── parseMelt: exportable fields map through ─────────────────────

TEST(MeltParserTests, parseMeltCapturesAllExportableFields) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": {
            "dependencies": {
                "cajeta.io.net.http": "1.2.5",
                "x.y.z":              ">=1.0.0,<2.0.0"
            },
            "properties": {
                "platform-version": "2024.1",
                "release-tag":      "v${platform-version}-stable"
            },
            "actions": {
                "ship-to-prod": {
                    "extends": "upload",
                    "params":  { "target": "s3" }
                }
            },
            "repositories": [
                { "name": "platform-internal",
                  "url":  "https://nexus.example.com/cajeta",
                  "priority": 150 }
            ],
            "melts": [
                "cajeta.platform.lang-melt@1.0.0"
            ]
        }
    })");

    auto melt = parseMelt(m);
    ASSERT_TRUE((bool)melt) << errorText(melt.takeError());

    EXPECT_EQ(melt->dependencies.size(), 2u);
    EXPECT_EQ(melt->dependencies["cajeta.io.net.http"], "1.2.5");
    EXPECT_EQ(melt->dependencies["x.y.z"], ">=1.0.0,<2.0.0");

    EXPECT_EQ(melt->properties.size(), 2u);
    EXPECT_EQ(melt->properties["platform-version"], "2024.1");

    EXPECT_FALSE(melt->actionsRaw.empty());
    EXPECT_TRUE(melt->actionsRaw.getObject("ship-to-prod") != nullptr);

    ASSERT_EQ(melt->repositories.size(), 1u);
    EXPECT_EQ(melt->repositories[0].name, "platform-internal");
    EXPECT_EQ(melt->repositories[0].priority, 150);

    ASSERT_EQ(melt->melts.size(), 1u);
    EXPECT_EQ(melt->melts[0].name, "cajeta.platform.lang-melt");
    EXPECT_EQ(melt->melts[0].version, "1.0.0");
}

TEST(MeltParserTests, parseMeltReturnsEmptyWhenNoMeltBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" }
    })");
    auto melt = parseMelt(m);
    ASSERT_TRUE((bool)melt) << errorText(melt.takeError());
    EXPECT_TRUE(melt->dependencies.empty());
    EXPECT_TRUE(melt->properties.empty());
    EXPECT_TRUE(melt->melts.empty());
}

// ─── parseMelt: non-exportable fields rejected ────────────────────

TEST(MeltParserTests, parseMeltRejectsNonExportableField) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": {
            "dependencies": {},
            "plugins": ["cajeta.coverage"]
        }
    })");
    auto melt = parseMelt(m);
    ASSERT_FALSE((bool)melt);
    auto msg = errorText(melt.takeError());
    EXPECT_NE(msg.find("'melt.plugins' is not an exportable"),
              std::string::npos);
}

TEST(MeltParserTests, parseMeltRejectsNonStringPropertyValue) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": {
            "properties": { "bad": 42 }
        }
    })");
    auto melt = parseMelt(m);
    ASSERT_FALSE((bool)melt);
    auto msg = errorText(melt.takeError());
    EXPECT_NE(msg.find("must be a string"), std::string::npos);
}

TEST(MeltParserTests, parseMeltRejectsNonStringDependencyConstraint) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "melt": {
            "dependencies": { "x.y": { "version": "1.0.0" } }
        }
    })");
    auto melt = parseMelt(m);
    ASSERT_FALSE((bool)melt);
    auto msg = errorText(melt.takeError());
    EXPECT_NE(msg.find("must be a version constraint string"),
              std::string::npos);
}

// ─── parseSettingsMelts ───────────────────────────────────────────

TEST(MeltParserTests, parseSettingsMeltsParsesPins) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "settings": {
            "melts": [
                "com.example.platform-melt@2024.1.0",
                "com.example.test-melt@1.0.0"
            ]
        }
    })");
    auto imports = parseSettingsMelts(m);
    ASSERT_TRUE((bool)imports) << errorText(imports.takeError());
    ASSERT_EQ(imports->size(), 2u);
    EXPECT_EQ((*imports)[0].name, "com.example.platform-melt");
    EXPECT_EQ((*imports)[0].version, "2024.1.0");
    EXPECT_EQ((*imports)[1].name, "com.example.test-melt");
    EXPECT_EQ((*imports)[1].version, "1.0.0");
}

TEST(MeltParserTests, parseSettingsMeltsEmptyWhenNoBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" }
    })");
    auto imports = parseSettingsMelts(m);
    ASSERT_TRUE((bool)imports) << errorText(imports.takeError());
    EXPECT_TRUE(imports->empty());
}

TEST(MeltParserTests, parseSettingsMeltsRejectsMalformedEntry) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "settings": {
            "melts": [ "no-at-sign" ]
        }
    })");
    auto imports = parseSettingsMelts(m);
    ASSERT_FALSE((bool)imports);
    EXPECT_NE(errorText(imports.takeError()).find("name@version"),
              std::string::npos);
}

TEST(MeltParserTests, parseSettingsMeltsRejectsNonStringEntry) {
    auto m = mustLoad(R"({
        "details": { "name": "p", "version": "1.0.0" },
        "settings": {
            "melts": [ { "name": "x" } ]
        }
    })");
    auto imports = parseSettingsMelts(m);
    ASSERT_FALSE((bool)imports);
    EXPECT_NE(errorText(imports.takeError()).find("must be a string"),
              std::string::npos);
}

// ─── parseMeltImport ──────────────────────────────────────────────

TEST(MeltParserTests, parseMeltImportSplitsNameAndVersion) {
    auto r = parseMeltImport("com.example.melt@2024.1.0");
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->name, "com.example.melt");
    EXPECT_EQ(r->version, "2024.1.0");
}

TEST(MeltParserTests, parseMeltImportUsesLastAt) {
    // Names don't contain '@' today, but if a registry ever lets one
    // through, splitting on the rightmost '@' keeps the version part
    // intact.
    auto r = parseMeltImport("name@with@1.0.0");
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->name, "name@with");
    EXPECT_EQ(r->version, "1.0.0");
}

TEST(MeltParserTests, parseMeltImportRejectsMissingAt) {
    auto r = parseMeltImport("no-at-sign");
    ASSERT_FALSE((bool)r);
}

TEST(MeltParserTests, parseMeltImportRejectsEmptyHalves) {
    auto a = parseMeltImport("@1.0.0");
    ASSERT_FALSE((bool)a);
    consumeError(a.takeError());

    auto b = parseMeltImport("name@");
    ASSERT_FALSE((bool)b);
    consumeError(b.takeError());
}
