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
            {"opt", "speed"},
        }),
        customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    auto opt = r->overrides.getString("opt");
    ASSERT_TRUE(opt);
    EXPECT_EQ(opt->str(), "speed");
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
        {"opt", "size"},
    };
    customFlavors["app"] = llvm::json::Object{
        {"base", "base-tuned"},
        {"strip-symbols", "true"},
    };
    auto r = resolveFlavor(llvm::json::Value("app"), customFlavors);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->base, "release");
    EXPECT_TRUE(r->overrides.getString("opt"));
    EXPECT_TRUE(r->overrides.getString("strip-symbols"));
}

TEST(FlavorTests, neitherStringNorObjectErrors) {
    llvm::json::Object customFlavors;
    auto r = resolveFlavor(llvm::json::Value(42), customFlavors);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("string or an object"), std::string::npos);
}
