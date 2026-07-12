// Regression tests for Phase 5b custom-flavor map composition.
//
// Pins:
//   - Built-in string flavor (release/debug) resolves to itself.
//   - Custom-flavor name walks the base chain to a built-in.
//   - Inline { base, ...overrides } composes overrides on top.
//   - Cycle detection (A→B→A) errors.
//   - Unknown custom name errors.
//   - Override-by-override map merge: deeper levels lose to
//     shallower ones (the closer-to-the-task-author wins).

#include "cajeta/buildtool/Flavor.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <string>

using cajeta::buildtool::resolveFlavor;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

} // namespace

TEST(FlavorTests, builtInStringResolvesToItself) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value("release"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    EXPECT_TRUE(r->overrides.empty());
}

TEST(FlavorTests, debugBuiltInResolves) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value("debug"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "debug");
}

TEST(FlavorTests, customFlavorNameWalksToBuiltIn) {
    llvm::json::Object customFlavors;
    customFlavors["integration"] = llvm::json::Object{
        {"base", "release"},
        {"debug-info", "full"},
    };
    auto r = resolveFlavor(llvm::json::Value("integration"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    auto di = r->overrides.getString("debug-info");
    ASSERT_TRUE(di);
    EXPECT_EQ(di->str(), "full");
}

TEST(FlavorTests, inlineMapComposesOverrides) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(
        llvm::json::Value(llvm::json::Object{
            {"base", "release"},
            {"opt", "O3"},
        }),
        customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    auto opt = r->overrides.getString("opt");
    ASSERT_TRUE(opt);
    EXPECT_EQ(opt->str(), "O3");
}

TEST(FlavorTests, unknownCustomNameErrors) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value("totally-made-up"),
                           customFlavors);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not a built-in"), std::string::npos);
}

TEST(FlavorTests, cyclicChainErrors) {
    llvm::json::Object customFlavors;
    customFlavors["a"] = llvm::json::Object{{"base", "b"}};
    customFlavors["b"] = llvm::json::Object{{"base", "a"}};
    auto r = resolveFlavor(llvm::json::Value("a"), customFlavors);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("cycle"), std::string::npos);
}

TEST(FlavorTests, missingBaseInCustomErrors) {
    llvm::json::Object customFlavors;
    customFlavors["bad"] = llvm::json::Object{
        {"debug-info", "full"},
    };
    auto r = resolveFlavor(llvm::json::Value("bad"), customFlavors);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("missing required 'base'"), std::string::npos);
}

// ─── Phase 8: property vocabulary + load-time validation ──────────

TEST(FlavorTests, vocabularyHasAllSixteenKeys) {
    using cajeta::buildtool::flavorPropertyVocab;
    const auto& v = flavorPropertyVocab();
    EXPECT_EQ(v.size(), 16u);
    // Spot-check a key from each major group: optimization, target,
    // safety, sanitizer, instrumentation.
    bool sawOpt = false, sawCpu = false, sawBoundsCheck = false,
         sawAsan = false, sawSourceTags = false;
    for (const auto& s : v) {
        if (s.key == "opt") sawOpt = true;
        if (s.key == "cpu") sawCpu = true;
        if (s.key == "bounds-check") sawBoundsCheck = true;
        if (s.key == "asan") sawAsan = true;
        if (s.key == "source-tags") sawSourceTags = true;
    }
    EXPECT_TRUE(sawOpt);
    EXPECT_TRUE(sawCpu);
    EXPECT_TRUE(sawBoundsCheck);
    EXPECT_TRUE(sawAsan);
    EXPECT_TRUE(sawSourceTags);
}

TEST(FlavorTests, findKnownPropertyReturnsSpec) {
    using cajeta::buildtool::findFlavorPropertySpec;
    auto* opt = findFlavorPropertySpec("opt");
    ASSERT_TRUE(opt);
    EXPECT_EQ(opt->kind,
              cajeta::buildtool::FlavorPropertySpec::Kind::EnumString);
    EXPECT_EQ(opt->allowed.size(), 5u);

    auto* asan = findFlavorPropertySpec("asan");
    ASSERT_TRUE(asan);
    EXPECT_EQ(asan->kind,
              cajeta::buildtool::FlavorPropertySpec::Kind::Boolean);
}

TEST(FlavorTests, findUnknownPropertyReturnsNull) {
    using cajeta::buildtool::findFlavorPropertySpec;
    EXPECT_EQ(findFlavorPropertySpec("debg-info"), nullptr);
    EXPECT_EQ(findFlavorPropertySpec(""), nullptr);
}

TEST(FlavorTests, validatePropertyAcceptsCorrectValues) {
    using cajeta::buildtool::validateFlavorProperty;
    EXPECT_FALSE((bool)validateFlavorProperty(
        "opt", llvm::json::Value("O2"), "test"));
    EXPECT_FALSE((bool)validateFlavorProperty(
        "asan", llvm::json::Value(true), "test"));
    EXPECT_FALSE((bool)validateFlavorProperty(
        "bounds-check", llvm::json::Value("trap"), "test"));
}

TEST(FlavorTests, validatePropertyRejectsUnknownKey) {
    using cajeta::buildtool::validateFlavorProperty;
    auto e = validateFlavorProperty(
        "debg-info", llvm::json::Value("full"), "where");
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("unknown property key"), std::string::npos);
    EXPECT_NE(msg.find("debg-info"), std::string::npos);
    EXPECT_NE(msg.find("where"), std::string::npos);
    // Citation must list the allowed vocabulary.
    EXPECT_NE(msg.find("debug-info"), std::string::npos);
}

TEST(FlavorTests, validatePropertyRejectsOutOfRangeEnum) {
    using cajeta::buildtool::validateFlavorProperty;
    auto e = validateFlavorProperty(
        "opt", llvm::json::Value("speed"), "test");
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("speed"), std::string::npos);
    EXPECT_NE(msg.find("O2"), std::string::npos);
}

TEST(FlavorTests, validatePropertyRejectsWrongValueType) {
    using cajeta::buildtool::validateFlavorProperty;
    auto e = validateFlavorProperty(
        "asan", llvm::json::Value("true"), "test");
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("boolean"), std::string::npos);
}

TEST(FlavorTests, builtinReleasePropertiesMatchSpec) {
    using cajeta::buildtool::builtinFlavorProperties;
    auto r = builtinFlavorProperties("release");
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->getString("opt")->str(), "O2");
    EXPECT_EQ(r->getString("lto")->str(), "thin");
    EXPECT_EQ(*r->getBoolean("strip-symbols"), true);
    // external-debug 1.1.5: `line`, not `off`. Now that the property actually
    // lowers, `off` would strip the shadow stack from release builds and change
    // their output — and semantic exception traces are the ergonomic default
    // (spec §2.2.2/§2.2.3). `line` is exactly today's release codegen.
    EXPECT_EQ(r->getString("debug-info")->str(), "line");
    EXPECT_EQ(r->getString("bounds-check")->str(), "off");
}

TEST(FlavorTests, builtinDebugPropertiesMatchSpec) {
    using cajeta::buildtool::builtinFlavorProperties;
    auto r = builtinFlavorProperties("debug");
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->getString("opt")->str(), "O0");
    EXPECT_EQ(r->getString("lto")->str(), "off");
    EXPECT_EQ(*r->getBoolean("strip-symbols"), false);
    EXPECT_EQ(r->getString("debug-info")->str(), "full");
    EXPECT_EQ(r->getString("bounds-check")->str(), "on");
}

TEST(FlavorTests, builtinUnknownNameErrors) {
    using cajeta::buildtool::builtinFlavorProperties;
    auto r = builtinFlavorProperties("nope");
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not a built-in"), std::string::npos);
}

TEST(FlavorTests, effectivePropertiesMergesBuiltInAndOverrides) {
    using cajeta::buildtool::effectiveProperties;
    using cajeta::buildtool::ResolvedFlavor;
    ResolvedFlavor rf;
    rf.base = "release";
    rf.overrides["debug-info"] = "full";
    rf.overrides["asan"]       = true;
    auto eff = effectiveProperties(rf);
    ASSERT_TRUE((bool)eff) << errorText(eff.takeError());
    // Override beats default.
    EXPECT_EQ(eff->getString("debug-info")->str(), "full");
    // Override key new to the bundle is added.
    EXPECT_EQ(*eff->getBoolean("asan"), true);
    // Untouched default is preserved.
    EXPECT_EQ(eff->getString("opt")->str(), "O2");
}

TEST(FlavorTests, toCompilerFlagsRendersDeterministically) {
    using cajeta::buildtool::toCompilerFlags;
    llvm::json::Object props{
        {"opt",           "O2"},
        {"asan",          true},    // no compiler frontend flag — dropped
        {"strip-symbols", false},   // no compiler frontend flag — dropped
        {"bounds-check",  "off"},   // lowers to --bounds
        {"source-tags",   true},    // boolean → --source-tags=on
    };
    auto flags = toCompilerFlags(props);
    // Only properties that map to a compiler frontend flag are lowered, in
    // vocabulary order, using the MAPPED flag name (bounds-check -> bounds).
    // strip-symbols / sanitizers / analytics have no frontend flag and are
    // honored at the emit/link stage instead.
    ASSERT_EQ(flags.size(), 3u);
    EXPECT_EQ(flags[0], "--opt=O2");
    EXPECT_EQ(flags[1], "--bounds=off");
    EXPECT_EQ(flags[2], "--source-tags=on");
}

// external-debug 1.1.4 — `debug-info` used to map to an EMPTY compiler flag, so
// a flavor asking for `full` was silently dropped and the binary carried no
// debug records at all (spec §1.2, §2.1.2). It lowers now.
TEST(FlavorTests, debugInfoLowersToCompilerFlag) {
    using cajeta::buildtool::findFlavorPropertySpec;
    using cajeta::buildtool::toCompilerFlags;

    const auto* spec = findFlavorPropertySpec("debug-info");
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->compilerFlag, "debug-info");

    for (const char* level : {"off", "line", "full"}) {
        llvm::json::Object props{{"debug-info", level}};
        auto flags = toCompilerFlags(props);
        ASSERT_EQ(flags.size(), 1u) << level;
        EXPECT_EQ(flags[0], std::string("--debug-info=") + level);
    }
}

// benchmark-fidelity Unit 3 (3.b.3): xpu-backend lowers to --xpu-backend=<v> so
// a manifest flavor can build @Kernel methods for the CPU backend (profile suite
// matmul). EnumString, vocabulary-ordered after cpu / before lto.
TEST(FlavorTests, xpuBackendLowersToCompilerFlag) {
    using cajeta::buildtool::toCompilerFlags;
    llvm::json::Object props{
        {"opt",         "O3"},
        {"cpu",         "native"},
        {"xpu-backend", "cpu"},
    };
    auto flags = toCompilerFlags(props);
    ASSERT_EQ(flags.size(), 3u);
    EXPECT_EQ(flags[0], "--opt=O3");
    EXPECT_EQ(flags[1], "--cpu=native");
    EXPECT_EQ(flags[2], "--xpu-backend=cpu");
}

TEST(FlavorTests, xpuBackendSpecIsKnownEnum) {
    using cajeta::buildtool::findFlavorPropertySpec;
    const auto* spec = findFlavorPropertySpec("xpu-backend");
    ASSERT_TRUE(spec);
    EXPECT_EQ(spec->kind,
              cajeta::buildtool::FlavorPropertySpec::Kind::EnumStringCsv);
    EXPECT_EQ(spec->allowed.size(), 5u);  // none/cpu/vulkan/nvptx/amdgpu
}

TEST(FlavorTests, validateCustomFlavorsAcceptsValidChain) {
    using cajeta::buildtool::validateCustomFlavors;
    llvm::json::Object cf;
    cf["integration"] = llvm::json::Object{
        {"base",       "release"},
        {"debug-info", "full"},
        {"analytics",  true},
    };
    cf["asan-debug"] = llvm::json::Object{
        {"base", "debug"},
        {"asan", true},
    };
    EXPECT_FALSE((bool)validateCustomFlavors(cf));
}

TEST(FlavorTests, validateCustomFlavorsRejectsUnknownKey) {
    using cajeta::buildtool::validateCustomFlavors;
    llvm::json::Object cf;
    cf["typo"] = llvm::json::Object{
        {"base",       "release"},
        {"debg-info",  "full"},
    };
    auto e = validateCustomFlavors(cf);
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("debg-info"), std::string::npos);
    EXPECT_NE(msg.find("custom-flavors.typo"), std::string::npos);
}

TEST(FlavorTests, validateCustomFlavorsRejectsCycle) {
    using cajeta::buildtool::validateCustomFlavors;
    llvm::json::Object cf;
    cf["a"] = llvm::json::Object{{"base", "b"}};
    cf["b"] = llvm::json::Object{{"base", "a"}};
    auto e = validateCustomFlavors(cf);
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("cycle"), std::string::npos);
}

TEST(FlavorTests, validateCustomFlavorsRejectsBadBaseType) {
    using cajeta::buildtool::validateCustomFlavors;
    llvm::json::Object cf;
    cf["weird"] = llvm::json::Object{
        {"base", 42},
    };
    auto e = validateCustomFlavors(cf);
    ASSERT_TRUE((bool)e);
    auto msg = errorText(std::move(e));
    EXPECT_NE(msg.find("base"), std::string::npos);
    EXPECT_NE(msg.find("string"), std::string::npos);
}

TEST(FlavorTests, resolveFlavorInlineRejectsBadKey) {
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
    EXPECT_NE(msg.find("inline flavor"), std::string::npos);
}

TEST(FlavorTests, inlineWithoutBaseErrors) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(
        llvm::json::Value(llvm::json::Object{
            {"opt", "speed"},
        }),
        customFlavors);
    ASSERT_FALSE((bool)r);
}

TEST(FlavorTests, chainedCustomFlavorsCompose) {
    llvm::json::Object customFlavors;
    customFlavors["base-tuned"] = llvm::json::Object{
        {"base", "release"},
        {"opt", "Oz"},
    };
    customFlavors["app"] = llvm::json::Object{
        {"base", "base-tuned"},
        {"strip-symbols", true},
    };
    auto r = resolveFlavor(llvm::json::Value("app"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    EXPECT_TRUE(r->overrides.getString("opt"));
    auto strip = r->overrides.getBoolean("strip-symbols");
    ASSERT_TRUE(strip);
    EXPECT_TRUE(*strip);
}

TEST(FlavorTests, neitherStringNorObjectErrors) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value(42), customFlavors);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("string or an object"), std::string::npos);
}
