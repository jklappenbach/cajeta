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


TEST(FlavorTests, debugBuiltInResolves) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value("debug"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "debug");
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


// ─── Phase 8: property vocabulary + load-time validation ──────────






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



TEST(FlavorTests, builtinUnknownNameErrors) {
    using cajeta::buildtool::builtinFlavorProperties;
    auto r = builtinFlavorProperties("nope");
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not a built-in"), std::string::npos);
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








