// Regression tests for the build-tool property resolver.
// See src/cajeta/buildtool/Properties.h and
// plan/build-tool-plan.md Phase 1.

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <string>

#include "../PortableEnv.h"

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseCliOverride;
using cajeta::buildtool::PropertyOverrides;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::substitute;

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
            ADD_FAILURE() << "manifest load failed: "
                          << errorText(m.takeError());
            return {};
        }
        return std::move(*m);
    }

} // namespace

// --- built-ins ------------------------------------------------------------

TEST(PropertiesTests, builtinsResolveFromDetailsBlock) {
    auto m = mustLoad(R"({
        "details": { "name": "com.example.foo", "version": "1.2.3" }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    // Built-ins aren't user-declared, so they appear in the values map
    // only when referenced. To exercise them, substitute a string that
    // references each.
    auto out = substitute(
        "${details.name} ${details.version} ${details.group} ${details.library}",
        *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "com.example.foo 1.2.3 com.example foo");
}

TEST(PropertiesTests, builtinFlavorTargetProfileComeFromOverrides) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    PropertyOverrides ov;
    ov.flavor = "release";
    ov.target = "x86_64-linux-gnu";
    ov.profile = "ci";
    auto r = resolveProperties(m, ov);
    ASSERT_TRUE((bool)r);
    auto out = substitute("${flavor}|${target}|${profile}", *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "release|x86_64-linux-gnu|ci");
}

TEST(PropertiesTests, builtinEnvReadsFromProcess) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    setenv("PROP_TEST_ENV_FOO", "hello-world", 1);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    auto out = substitute("${env.PROP_TEST_ENV_FOO}", *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "hello-world");
    unsetenv("PROP_TEST_ENV_FOO");
}

TEST(PropertiesTests, builtinEnvMissingResolvesToEmpty) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    unsetenv("PROP_TEST_ENV_DEFINITELY_MISSING");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    auto out = substitute("[${env.PROP_TEST_ENV_DEFINITELY_MISSING}]", *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "[]");
}

// --- user-defined properties ---------------------------------------------

TEST(PropertiesTests, userPropertiesResolveDirectly) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "x": "hello" }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["x"], "hello");
}

TEST(PropertiesTests, userPropertiesReferenceOtherUserProperties) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": {
            "major":   "1.4",
            "minor":   "7",
            "version": "${major}.${minor}"
        }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["version"], "1.4.7");
}

TEST(PropertiesTests, userPropertiesReferenceBuiltins) {
    auto m = mustLoad(R"({
        "details": { "name": "com.example.foo", "version": "0.1.0" },
        "properties": {
            "release-prefix": "cajeta/${details.name}/v${details.version}/"
        }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["release-prefix"],
              "cajeta/com.example.foo/v0.1.0/");
}

TEST(PropertiesTests, topologicalResolutionAcrossThreeLevels) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": {
            "a": "1",
            "b": "${a}+2",
            "c": "${b}+3"
        }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["c"], "1+2+3");
}

// --- override precedence -------------------------------------------------

TEST(PropertiesTests, cliOverrideBeatsManifest) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "v": "manifest-value" }
    })");
    PropertyOverrides ov;
    ov.cli["v"] = "cli-value";
    auto r = resolveProperties(m, ov);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["v"], "cli-value");
}

TEST(PropertiesTests, cliOverrideBeatsEnv) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "v": "manifest-value" }
    })");
    PropertyOverrides ov;
    ov.env["v"] = "env-value";
    ov.cli["v"] = "cli-value";
    auto r = resolveProperties(m, ov);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["v"], "cli-value");
}

TEST(PropertiesTests, envOverrideBeatsManifest) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "v": "manifest-value" }
    })");
    PropertyOverrides ov;
    ov.env["v"] = "env-value";
    auto r = resolveProperties(m, ov);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["v"], "env-value");
}

TEST(PropertiesTests, overridesPropagateThroughReferences) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": {
            "v":        "1.0.0",
            "qualified": "x@${v}"
        }
    })");
    PropertyOverrides ov;
    ov.cli["v"] = "2.0.0";
    auto r = resolveProperties(m, ov);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->values["v"], "2.0.0");
    EXPECT_EQ(r->values["qualified"], "x@2.0.0");
}

// --- error cases ---------------------------------------------------------

TEST(PropertiesTests, errorsOnCyclicReference) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "x": "${y}", "y": "${x}" }
    })");
    auto r = resolveProperties(m);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("cyclic property reference"), std::string::npos);
}

TEST(PropertiesTests, errorsOnUndefinedReference) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "x": "${doesnotexist}" }
    })");
    auto r = resolveProperties(m);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("undefined property 'doesnotexist'"),
              std::string::npos);
}

TEST(PropertiesTests, errorsOnUserPropertyCollidingWithBuiltin) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "details.name": "hijack" }
    })");
    auto r = resolveProperties(m);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("collides with a built-in"), std::string::npos);
}

TEST(PropertiesTests, errorsOnEnvNamespaceCollision) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "env.SECRET": "leak" }
    })");
    auto r = resolveProperties(m);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("collides with a built-in"), std::string::npos);
}

TEST(PropertiesTests, errorsOnNonStringPropertyValue) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "x": 42 }
    })");
    auto r = resolveProperties(m);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("must be a string"), std::string::npos);
}

// --- substitute() helper -------------------------------------------------

TEST(PropertiesTests, substituteResolvesMultipleReferences) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" },
        "properties": { "a": "X", "b": "Y" }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    auto out = substitute("${a}-${b}-${a}", *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "X-Y-X");
}

TEST(PropertiesTests, substituteHonorsDollarDollarEscape) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    auto out = substitute("$${not-a-property}", *r);
    ASSERT_TRUE((bool)out);
    EXPECT_EQ(*out, "${not-a-property}");
}

TEST(PropertiesTests, substituteErrorsOnUnknownReference) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    auto out = substitute("${not-defined}", *r, "test-context");
    ASSERT_FALSE((bool)out);
    auto msg = errorText(out.takeError());
    EXPECT_NE(msg.find("test-context"), std::string::npos);
    EXPECT_NE(msg.find("not-defined"), std::string::npos);
}

// --- CLI override parsing -------------------------------------------------

TEST(PropertiesTests, parseCliOverrideBasic) {
    auto r = parseCliOverride("name=value");
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->first, "name");
    EXPECT_EQ(r->second, "value");
}

TEST(PropertiesTests, parseCliOverrideAllowsEqualsInValue) {
    auto r = parseCliOverride("flag=a=b=c");
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->first, "flag");
    EXPECT_EQ(r->second, "a=b=c");
}

TEST(PropertiesTests, parseCliOverrideRejectsMissingEquals) {
    auto r = parseCliOverride("just-a-name");
    ASSERT_FALSE((bool)r);
    if (!r) consumeError(r.takeError());
}

TEST(PropertiesTests, parseCliOverrideRejectsEmptyName) {
    auto r = parseCliOverride("=value");
    ASSERT_FALSE((bool)r);
    if (!r) consumeError(r.takeError());
}
