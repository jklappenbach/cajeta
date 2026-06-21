// Regression tests for Phase 5a's BuildAction and the
// settings.build parsing it depends on. These tests exercise
// the resolution logic (entry-method / binary / settings.build
// defaults; emit defaulting; settings.build.binaries parsing)
// without actually invoking the compiler — that integration is
// covered by smoke tests against real source trees.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <string>

using cajeta::buildtool::ActionRegistry;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::projectRootFromManifest;
using cajeta::buildtool::parseSettingsBuild;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::TaskContext;

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

// ─── settings.build parsing ────────────────────────────────────────────

TEST(BuildActionTests, settingsBuildParsesEmptyToDefaults) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto sb = parseSettingsBuild(m);
    ASSERT_TRUE((bool)sb);
    EXPECT_FALSE(sb->entryMethod.has_value());
    EXPECT_FALSE(sb->target.has_value());
    EXPECT_TRUE(sb->binaries.empty());
}

TEST(BuildActionTests, settingsBuildParsesSingleEntryMethod) {
    auto m = mustLoad(R"({
        "details":  { "name": "a.b", "version": "0.1" },
        "settings": {
            "build": {
                "entry-method": "com.example.Main::main",
                "target":       "host"
            }
        }
    })");
    auto sb = parseSettingsBuild(m);
    ASSERT_TRUE((bool)sb);
    EXPECT_EQ(sb->entryMethod.value_or(""), "com.example.Main::main");
    EXPECT_EQ(sb->target.value_or(""), "host");
}

TEST(BuildActionTests, settingsBuildParsesBinariesRegistry) {
    auto m = mustLoad(R"({
        "details":  { "name": "a.b", "version": "0.1" },
        "settings": {
            "build": {
                "binaries": {
                    "server":  { "entry-method": "com.example.api.server.Main::main",
                                 "description":  "Production HTTP server" },
                    "migrate": { "entry-method": "com.example.api.migrate.Main::main" },
                    "diag":    { "entry-method": "com.example.api.diag.Main::main" }
                }
            }
        }
    })");
    auto sb = parseSettingsBuild(m);
    ASSERT_TRUE((bool)sb);
    EXPECT_EQ(sb->binaries.size(), 3u);
    EXPECT_EQ(sb->binaries.at("server").entryMethod,
              "com.example.api.server.Main::main");
    EXPECT_EQ(sb->binaries.at("server").description.value_or(""),
              "Production HTTP server");
    EXPECT_EQ(sb->binaries.at("diag").entryMethod,
              "com.example.api.diag.Main::main");
}

TEST(BuildActionTests, settingsBuildErrorsOnBinariesEntryMissingEntryMethod) {
    auto m = mustLoad(R"({
        "details":  { "name": "a.b", "version": "0.1" },
        "settings": {
            "build": {
                "binaries": {
                    "broken": { "description": "missing entry-method" }
                }
            }
        }
    })");
    auto sb = parseSettingsBuild(m);
    ASSERT_FALSE((bool)sb);
    auto msg = errorText(sb.takeError());
    EXPECT_NE(msg.find("missing required 'entry-method'"),
              std::string::npos);
}

// ─── build action entry-method resolution ─────────────────────────────

namespace {

    cajeta::buildtool::ResolvedProperties makeProps(
        const cajeta::buildtool::Manifest& m) {
        auto r = resolveProperties(m);
        if (!r) {
            ADD_FAILURE() << errorText(r.takeError());
            return {};
        }
        return std::move(*r);
    }

    // Invoke BuildAction with given params. Since this would fork
    // the cajeta compiler, we don't actually run it — we just want
    // to verify the resolution + validation path. To make that
    // testable without forking, we craft inputs that should fail
    // BEFORE the fork (in resolution) and check the resulting
    // error message.
    llvm::Error invokeAndExpectError(
        const ActionRegistry& registry,
        TaskContext& ctx,
        const llvm::json::Object& params) {
        auto r = registry.get("build")->run(params, ctx);
        if (!r) return r.takeError();
        // If the action actually ran (it'd shell out to the
        // compiler), the test setup is wrong — caller should
        // ensure the inputs short-circuit before fork. Fail loudly.
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "test expected resolution failure but action ran "
            "successfully (this means the test's inputs let the "
            "fork+exec proceed; tighten them)");
    }

} // namespace

TEST(BuildActionTests, errorWhenExecutableEmitHasNoEntryMethod) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    llvm::json::Object params;
    params["emit"] = "executable";
    auto err = invokeAndExpectError(registry, ctx, params);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("emit='executable' requires an entry method"),
              std::string::npos);
}

TEST(BuildActionTests, errorWhenBinaryNotFoundInRegistry) {
    auto m = mustLoad(R"({
        "details":  { "name": "a.b", "version": "0.1" },
        "settings": {
            "build": {
                "binaries": {
                    "server":  { "entry-method": "x.Server::main" },
                    "migrate": { "entry-method": "x.Migrate::main" }
                }
            }
        }
    })");
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    llvm::json::Object params;
    params["binary"] = "ghost";
    auto err = invokeAndExpectError(registry, ctx, params);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("'ghost' not found"), std::string::npos);
    EXPECT_NE(msg.find("server"),  std::string::npos);
    EXPECT_NE(msg.find("migrate"), std::string::npos);
}

TEST(BuildActionTests, errorWhenEmitIsInvalid) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, &m);

    llvm::json::Object params;
    params["emit"] = "ir";   // valid for compiler CLI, but not action API
    auto err = invokeAndExpectError(registry, ctx, params);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("must be one of"), std::string::npos);
    EXPECT_NE(msg.find("exploded-ir"), std::string::npos);
}

TEST(BuildActionTests, binaryParamRequiresManifest) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    auto props = makeProps(m);
    ActionRegistry registry;
    TaskContext ctx(props, nullptr);  // no manifest

    llvm::json::Object params;
    params["binary"] = "server";
    auto err = invokeAndExpectError(registry, ctx, params);
    auto msg = errorText(std::move(err));
    EXPECT_NE(msg.find("requires a manifest"), std::string::npos);
}

// ─── project-root derivation (skill-discovery D.3 regression) ──────────
//
// The build action embeds a package's hand-authored skills/ from the PROJECT
// root (the dir holding cajeta.json) and passes it via --skill-root. A prior
// bug pointed skill embedding at the deeper compile source root
// (src/main/cajeta), which has no skills/, so built .cja's carried no skills.
// Guard the derivation the fix depends on.

TEST(BuildActionTests, projectRootIsManifestDirNotSourceRoot) {
    auto m = mustLoad(R"({
        "details": { "name": "a.b", "version": "0.1" }
    })");
    m.sourcePath = "/home/u/myproj/cajeta.json";
    // The project root is where skills/ and cajeta.json live — NOT the deeper
    // compile source root.
    EXPECT_EQ(projectRootFromManifest(m), "/home/u/myproj");
    EXPECT_NE(projectRootFromManifest(m), "/home/u/myproj/src/main/cajeta");
}

TEST(BuildActionTests, projectRootDefaultsToDotWhenUnset) {
    cajeta::buildtool::Manifest m;            // empty sourcePath
    EXPECT_EQ(projectRootFromManifest(m), ".");
    m.sourcePath = "cajeta.json";             // bare filename, no parent dir
    EXPECT_EQ(projectRootFromManifest(m), ".");
}
