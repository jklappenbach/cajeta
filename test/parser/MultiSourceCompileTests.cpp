//
// Multi-file compilation through the JIT — exercises:
//
//   - CajetaJit::compile(map<string,string>, fqEntryClass) — the
//     multi-source overload that lays each (fqClass → source)
//     pair under one temp source-root, parses each as its own
//     module, runs the shared A3/A8/Phase 1/Phase 2 passes, then
//     merges the IR modules before handing to LLJIT.
//   - CajetaType::fromContext's import-aware short-name lookup:
//     when two classes share a short name across packages, the
//     consumer's `import a.b.Foo;` steers resolution.
//   - Cross-module DI: @Component in one file, @Inject in
//     another, resolveDependencyGraph builds the graph across.
//   - Forward references across files: a consumer file parsed
//     BEFORE its dependency works through the placeholder
//     mechanism. CajetaType::fromContext's miss path consults
//     the archive registry (populated by the ANTLR-based
//     pre-scan over every .cajeta file under the source root),
//     creates a placeholder CajetaClass for known-but-unparsed
//     names, and visitClassDeclaration later fills the same
//     placeholder in-place when the real declaration arrives.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::map<std::string, std::string>& sources,
               const std::string& fqEntryClass) {
    auto jit = CajetaJit::compile(sources, fqEntryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Compile multiple sources without going through the JIT — used
// for tests that only inspect post-parse state.
void compileMultiForInspection(Compiler& compiler,
                               const std::map<std::string, std::string>& sources) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_ms_inspect_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    for (auto& [fqClass, src] : sources) {
        std::filesystem::path rel;
        size_t start = 0;
        for (size_t i = 0; i <= fqClass.size(); ++i) {
            if (i == fqClass.size() || fqClass[i] == '.') {
                rel /= fqClass.substr(start, i - start);
                start = i + 1;
            }
        }
        rel += ".cajeta";
        auto full = base / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream out(full);
        out << src;
        out.close();
    }
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_ms_inspect_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    cajeta::prescanSourceRoot(base.string());
    using recursive_directory_iterator = std::filesystem::recursive_directory_iterator;
    for (const auto& entry : recursive_directory_iterator(base)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cajeta") continue;
        auto m = compiler.createModule(entry.path().string(),
                                        base.string(),
                                        archive.string());
        compiler.compile(m);
    }
}

} // namespace

// Cross-module DI through the JIT: Dep is a @Component in
// package `dep`; Service is a @Component in package `service`
// that imports Dep and @Injects it. The synthesized inject
// methods' cross-module calls go through a path the
// post-codegen merge handles cleanly.
TEST(MultiSourceCompileTests, crossModuleInjectResolves) {
    std::map<std::string, std::string> sources;
    sources["dep.Dep"] =
        "package dep;\n"
        "@Component public class Dep {\n"
        "    public int32 value;\n"
        "    public Dep() { value = 33; return; }\n"
        "}\n";
    sources["service.Service"] =
        "package service;\n"
        "import dep.Dep;\n"
        "@Component public class Service {\n"
        "    @Inject Dep dep;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        return s.dep.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(sources, "service.Service"), 33);
}

// Cross-file forward reference end-to-end. Consumer `app.App` is
// keyed alphabetically BEFORE provider `xyz.Provider`, so it
// parses first. When App's body references `Provider`, the
// archive pre-scan has already seen Provider declared in
// xyz.Provider's source, so CajetaType::fromContext creates a
// placeholder CajetaClass. xyz.Provider's later
// visitClassDeclaration finds the placeholder and fills it in
// via fillFromDeclaration. The placeholder's shared_ptr identity
// is the same as the eventual real class, so App's earlier
// reference stays valid.
//
// The cross-module merge that follows is the second piece: App's
// synthesized __cajeta_inject calls Provider's __cajeta_inject,
// which lives in Provider's llvm::Module. Without intervention
// the CallInst operand would dangle once Linker::linkModules
// rewrites symbols. CajetaModule::ensureFunctionVisible inserts a
// proper extern declaration in App's module so the call lands
// on a module-local Function* (which the linker resolves by
// symbol name at JIT load). Together those two pieces complete
// the forward-reference story.
TEST(MultiSourceCompileTests, forwardReferenceFillsPlaceholder) {
    std::map<std::string, std::string> sources;
    sources["app.App"] =
        "package app;\n"
        "import xyz.Provider;\n"
        "@Component public class App {\n"
        "    @Inject Provider p;\n"
        "    public App() { return; }\n"
        "    public static int32 run() {\n"
        "        App a = __cajeta_inject();\n"
        "        return a.p.value;\n"
        "    }\n"
        "}\n";
    sources["xyz.Provider"] =
        "package xyz;\n"
        "@Component public class Provider {\n"
        "    public int32 value;\n"
        "    public Provider() { value = 77; return; }\n"
        "}\n";
    EXPECT_EQ(runI32(sources, "app.App"), 77);
}

// Import-aware short-name resolution, inspected post-parse. Two
// classes named Logger live in different packages; a consumer
// file imports one. Verify the consumer module's imports map
// captured the right qualified name.
TEST(MultiSourceCompileTests, importMapCapturesQualifier) {
    std::map<std::string, std::string> sources;
    sources["fast.Logger"] =
        "package fast;\n"
        "public class Logger { public Logger() { return; } }\n";
    sources["slow.Logger"] =
        "package slow;\n"
        "public class Logger { public Logger() { return; } }\n";
    sources["app.App"] =
        "package app;\n"
        "import slow.Logger;\n"
        "public final class App { public App() { return; } }\n";
    Compiler compiler;
    compileMultiForInspection(compiler, sources);

    auto& s2m = CajetaModule::getStructureToModule();
    auto it = s2m.find("app.App");
    ASSERT_NE(it, s2m.end());
    auto appModule = it->second;
    ASSERT_NE(appModule, nullptr);
    auto& imports = appModule->getImports();
    auto importIt = imports.find("Logger");
    ASSERT_NE(importIt, imports.end());
    ASSERT_FALSE(importIt->second.empty());
    auto& packageMap = importIt->second;
    auto pkgIt = packageMap.find("slow");
    ASSERT_NE(pkgIt, packageMap.end());
    EXPECT_EQ(pkgIt->second->getTypeName(), "Logger");
    EXPECT_EQ(pkgIt->second->getPackageName(), "slow");
}

// Unknown type that's NOT in the archive throws — fromContext's
// miss path returns null (no archive entry) and the call site
// (visitFieldDeclaration) raises CAJETA_ERROR_UNKNOWN_TYPE. The
// placeholder synthesis is gated on archive presence, so typos
// and dead references stay as compile errors.

// `ArrayList<UserClass>` across files. Main is keyed alphabetically
// before DemoClass, so it parses first; when Main references
// `ArrayList<DemoClass>`, DemoClass is still a forward-ref
// placeholder. Pre-fix the placeholder-arg short-circuit in
// CajetaClass::instantiate returned the bare `ArrayList` template,
// dropping the type-args — `list.add` and `list.count` then
// dispatched against the template's unbound `T`-methods so
// `sizeCount` never incremented and the test returned 0 instead
// of 3. Fix: forward-ref placeholders (real package) proceed
// through normal instantiation; only T-var placeholders (empty
// package — set by the method-template visitor) short-circuit.
TEST(MultiSourceCompileTests, arrayListOfUserClassCrossFile) {
    std::map<std::string, std::string> sources;
    sources["test.Main"] =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        ArrayList<DemoClass> list = heap ArrayList<DemoClass>();\n"
        "        list.add(heap DemoClass(7));\n"
        "        list.add(heap DemoClass(11));\n"
        "        list.add(heap DemoClass(13));\n"
        "        return list.count();\n"
        "    }\n"
        "}\n";
    sources["test.DemoClass"] =
        "package test;\n"
        "public class DemoClass {\n"
        "    public int32 v;\n"
        "    public DemoClass(int32 x) { this.v = x; }\n"
        "}\n";
    EXPECT_EQ(runI32(sources, "test.Main"), 3);
}

// A user class whose SHORT name collides with an embedded-stdlib class
// (here `Uri`, colliding with `cajeta.io.net.uri.Uri`; the original
// regression hit `HttpRequest` before the http stack moved out to the
// external dev.cajeta.http library — the compiler path is identical)
// must not derail resolution in EITHER direction:
//
//   - stdlib code compiled alongside (App also imports PercentCodec,
//     pulling the cajeta.io.net.uri package into play; Uri.cajeta's own
//     methods reference `Uri` bare) — same-package references must not
//     bind the user's x.Uri;
//   - App's `import x.Uri;` must steer App's bare `Uri`
//     to the user class even though the stdlib class owns (or later
//     overwrites) the global short-name key in canonicalMap.
//
// Pre-fix, fromContext's FIRST lookup tier for a bare name was the
// global short-name key (toCanonical() of a package-less name IS the
// short name), so whichever same-named class registered last poisoned
// every other package's bare references — an unlocated
// CAJETA_ERROR_NULL_OPERAND out of stdlib internals. The fix
// resolves bare names own-package-first, then via explicit imports
// (including forward refs: an imported-but-unvisited canonical
// synthesizes its placeholder from the IMPORT, never the global
// short-name key), and only then falls back to the global tiers.
TEST(MultiSourceCompileTests, userClassShadowingStdlibShortName) {
    std::map<std::string, std::string> sources;
    sources["x.Uri"] =
        "package x;\n"
        "public class Uri {\n"
        "    public int32 v;\n"
        "    public Uri() { this.v = 7; }\n"
        "}\n";
    sources["app.App"] =
        "package app;\n"
        "import cajeta.lang.String;\n"
        "import x.Uri;\n"
        "import cajeta.io.net.uri.PercentCodec;\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        Uri r = heap Uri();\n"
        "        String d = PercentCodec.decode(\"a%20b\");\n"
        "        if (d.byteLength() != 3) return -1;\n"
        "        return r.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(sources, "app.App"), 7);
}
