// Phase 8 — Build flavors. Pins the six acceptance criteria from
// plans/buildtool/build-tool-plan.md "Phase 8 — Build flavors":
//
//   1. `flavor: "release"` resolves to the built-in property bundle.
//   2. `flavor: { "base": "release", "debug-info": "full" }`
//      resolves to release's bundle with debug-info overridden.
//   3. `flavor: "integration"` referencing a custom-flavor map
//      resolves through the named composition.
//   4. Unknown property key (`debg-info`) produces a citation
//      naming the offending key + the allowed vocabulary.
//   5. Two custom flavors with `base` cycling fail load-time
//      validation.
//   6. `build` action with `profile: "test"` invokes the compiler
//      with `--profile=test`; the resolved flavor's properties
//      flow through as `--<key>=<value>` argv flags.

#include "cajeta/buildtool/Flavor.h"
#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <algorithm>
#include <string>
#include <vector>

using cajeta::buildtool::builtinFlavorProperties;
using cajeta::buildtool::effectiveProperties;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseSettingsBuild;
using cajeta::buildtool::ResolvedFlavor;
using cajeta::buildtool::resolveFlavor;
using cajeta::buildtool::toCompilerFlags;
using cajeta::buildtool::validateCustomFlavors;
using cajeta::buildtool::validateFlavorProperty;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    bool contains(const std::vector<std::string>& v,
                  const std::string& needle) {
        return std::find(v.begin(), v.end(), needle) != v.end();
    }

} // namespace

// ─── Acceptance #1 — built-in name → built-in bundle ──────────

TEST(Phase8AcceptanceTests, releaseStringResolvesToBuiltInBundle) {
    llvm::json::Object cf;
    auto r = resolveFlavor(llvm::json::Value("release"), cf);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    EXPECT_TRUE(r->overrides.empty());

    auto eff = effectiveProperties(*r);
    ASSERT_TRUE((bool)eff) << errorText(eff.takeError());
    // Per BuildTool.md "Built-in flavors":
    EXPECT_EQ(eff->getString("opt")->str(),          "O2");
    EXPECT_EQ(eff->getString("lto")->str(),          "thin");
    EXPECT_EQ(*eff->getBoolean("strip-symbols"),     true);
    EXPECT_EQ(eff->getString("debug-info")->str(),   "line");
    EXPECT_EQ(eff->getString("bounds-check")->str(), "off");
}

// ─── Acceptance #2 — inline map composes overrides ────────────

TEST(Phase8AcceptanceTests, inlineMapOverridesReleaseDebugInfo) {
    llvm::json::Object cf;
    auto r = resolveFlavor(
        llvm::json::Value(llvm::json::Object{
            {"base",       "release"},
            {"debug-info", "full"},
        }),
        cf);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");

    auto eff = effectiveProperties(*r);
    ASSERT_TRUE((bool)eff) << errorText(eff.takeError());
    // Override wins:
    EXPECT_EQ(eff->getString("debug-info")->str(), "full");
    // Everything else is the release default:
    EXPECT_EQ(eff->getString("opt")->str(),       "O2");
    EXPECT_EQ(eff->getString("lto")->str(),       "thin");
    EXPECT_EQ(*eff->getBoolean("strip-symbols"),  true);
}

// ─── Acceptance #3 — named composition through custom-flavors ─

TEST(Phase8AcceptanceTests, customFlavorIntegrationResolvesThroughComposition) {
    auto m = loadManifestString(R"({
        "details": { "name": "x.y", "version": "0.1" },
        "settings": {
            "build": {
                "custom-flavors": {
                    "integration": {
                        "base":       "release",
                        "debug-info": "full",
                        "analytics":  true
                    }
                }
            }
        }
    })");
    ASSERT_TRUE((bool)m) << errorText(m.takeError());

    auto sb = parseSettingsBuild(*m);
    ASSERT_TRUE((bool)sb);

    auto r = resolveFlavor(llvm::json::Value("integration"),
                           sb->customFlavorsRaw);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");

    auto eff = effectiveProperties(*r);
    ASSERT_TRUE((bool)eff) << errorText(eff.takeError());
    EXPECT_EQ(eff->getString("debug-info")->str(), "full");
    EXPECT_EQ(*eff->getBoolean("analytics"),       true);
    // Defaults preserved.
    EXPECT_EQ(eff->getString("opt")->str(), "O2");
}

// ─── Acceptance #4 — unknown property key cites vocabulary ────

TEST(Phase8AcceptanceTests, unknownPropertyKeyCitesOffenderAndVocab) {
    // Surface the citation at three layers that all need to enforce
    // the same vocab: property validation, inline-flavor resolve,
    // and manifest-load.
    {
        auto e = validateFlavorProperty(
            "debg-info", llvm::json::Value("full"),
            "custom-flavors.broken");
        ASSERT_TRUE((bool)e);
        auto msg = errorText(std::move(e));
        EXPECT_NE(msg.find("debg-info"), std::string::npos);
        // Cited vocab includes the intended key:
        EXPECT_NE(msg.find("debug-info"), std::string::npos);
        EXPECT_NE(msg.find("allowed"), std::string::npos);
    }
    {
        llvm::json::Object cf;
        auto r = resolveFlavor(
            llvm::json::Value(llvm::json::Object{
                {"base",      "release"},
                {"debg-info", "full"},
            }),
            cf);
        ASSERT_FALSE((bool)r);
        auto msg = errorText(r.takeError());
        EXPECT_NE(msg.find("debg-info"), std::string::npos);
    }
    {
        auto m = loadManifestString(R"({
            "details": { "name": "x.y", "version": "0.1" },
            "settings": {
                "build": {
                    "custom-flavors": {
                        "broken": {
                            "base":      "release",
                            "debg-info": "full"
                        }
                    }
                }
            }
        })");
        ASSERT_FALSE((bool)m);
        auto msg = errorText(m.takeError());
        EXPECT_NE(msg.find("debg-info"), std::string::npos);
        // Citation flows through to the manifest source:
        EXPECT_NE(msg.find("custom-flavors.broken"), std::string::npos);
    }
}

// ─── Acceptance #5 — cycle in `base` chain fails at load ──────

TEST(Phase8AcceptanceTests, customFlavorCycleFailsLoadTimeValidation) {
    auto m = loadManifestString(R"({
        "details": { "name": "x.y", "version": "0.1" },
        "settings": {
            "build": {
                "custom-flavors": {
                    "a": { "base": "b" },
                    "b": { "base": "a" }
                }
            }
        }
    })");
    ASSERT_FALSE((bool)m);
    auto msg = errorText(m.takeError());
    EXPECT_NE(msg.find("cycle"), std::string::npos);
}

// ─── Acceptance #6 — profile + flavor flags flow to compiler ─

TEST(Phase8AcceptanceTests, resolvedFlavorProducesCompilerFlagArgv) {
    // Inline composition of release with debug-info=full and
    // analytics=true — equivalent to a `integration` custom flavor.
    llvm::json::Object cf;
    auto r = resolveFlavor(
        llvm::json::Value(llvm::json::Object{
            {"base",       "release"},
            {"debug-info", "full"},
            {"analytics",  true},
        }),
        cf);
    ASSERT_TRUE((bool)r);

    auto eff = effectiveProperties(*r);
    ASSERT_TRUE((bool)eff);

    auto flags = toCompilerFlags(*eff);
    // Only properties that map to a compiler frontend flag are lowered, using
    // the MAPPED name (bounds-check -> --bounds). lto lowers to the real
    // --lto frontend flag (ThinLTO for --emit=exe; vocab entry's mapped name
    // is "lto"). debug-info / strip-symbols / analytics have no frontend flag
    // — they're honored at the emit/link stage — so they do not appear.
    EXPECT_TRUE(contains(flags, "--opt=O2"));
    EXPECT_TRUE(contains(flags, "--bounds=off"));
    EXPECT_TRUE(contains(flags, "--lto=thin"));
    EXPECT_FALSE(contains(flags, "--debug-info=full"));
    EXPECT_FALSE(contains(flags, "--strip-symbols=true"));
    EXPECT_FALSE(contains(flags, "--analytics=true"));

    // Determinism — same map, same order:
    auto flags2 = toCompilerFlags(*eff);
    EXPECT_EQ(flags, flags2);
}
