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



// ─── mutual exclusion ─────────────────────────────────────────────


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
                "dev.cajeta.http": "1.2.5",
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
    EXPECT_EQ(melt->dependencies["dev.cajeta.http"], "1.2.5");
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



// ─── parseSettingsMelts ───────────────────────────────────────────



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




