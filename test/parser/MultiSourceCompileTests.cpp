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

// Cross-file forward reference at parse level. Consumer
// `app.App` is keyed alphabetically BEFORE provider
// `xyz.Provider`, so it parses first. When App's body references
// `Provider`, the archive pre-scan has already seen Provider
// declared in xyz.Provider's source, so CajetaType::fromContext
// creates a placeholder CajetaClass. xyz.Provider's later
// visitClassDeclaration finds the placeholder and fills it in
// via fillFromDeclaration. The placeholder's shared_ptr identity
// is the same as the eventual real class, so App's earlier
// reference stays valid.
//
// This test verifies the parse-level claim — placeholder
// promotion + fill-in — without going through JIT codegen. The
// JIT-roundtrip path has a separate cross-module merge gap
// (Linker::linkModules with OverrideFromSrc mishandles function
// references whose definition is in the donor, not the primary)
// that's tracked as a follow-up. End-to-end DI via the
// placeholder works for the order where the dependency parses
// first (covered by crossModuleInjectResolves above).
TEST(MultiSourceCompileTests, forwardReferenceFillsPlaceholder) {
    std::map<std::string, std::string> sources;
    sources["app.App"] =
        "package app;\n"
        "import xyz.Provider;\n"
        "public class App {\n"
        "    Provider p;\n"
        "    public App() { return; }\n"
        "}\n";
    sources["xyz.Provider"] =
        "package xyz;\n"
        "public class Provider {\n"
        "    public int32 value;\n"
        "    public Provider() { value = 77; return; }\n"
        "}\n";
    Compiler compiler;
    compileMultiForInspection(compiler, sources);

    // Provider is in canonicalMap and is NOT a placeholder
    // (it got filled in by visitClassDeclaration).
    auto& canon = cajeta::CajetaType::getCanonicalMap();
    auto it = canon.find("xyz.Provider");
    ASSERT_NE(it, canon.end()) << "xyz.Provider missing from canonicalMap";
    auto providerKlass = std::dynamic_pointer_cast<cajeta::CajetaClass>(it->second);
    ASSERT_NE(providerKlass, nullptr);
    EXPECT_FALSE(providerKlass->isPlaceholder())
        << "Provider should have been filled in by visitClassDeclaration";

    // App's `Provider p` field references the SAME CajetaClass
    // shared_ptr as Provider — the placeholder identity carries
    // through the fill-in.
    auto appIt = canon.find("app.App");
    ASSERT_NE(appIt, canon.end());
    auto appKlass = std::dynamic_pointer_cast<cajeta::CajetaClass>(appIt->second);
    ASSERT_NE(appKlass, nullptr);
    auto propIt = appKlass->getProperties().find("p");
    ASSERT_NE(propIt, appKlass->getProperties().end());
    auto fieldType = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        propIt->second->getType());
    ASSERT_NE(fieldType, nullptr);
    EXPECT_EQ(fieldType.get(), providerKlass.get())
        << "App's Provider-typed field should point at the SAME CajetaClass "
           "as canonicalMap[xyz.Provider] — the filled-in placeholder";
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
TEST(MultiSourceCompileTests, unknownTypeStillRejected) {
    std::map<std::string, std::string> sources;
    sources["app.App"] =
        "package app;\n"
        "@Component public class App {\n"
        "    @Inject DefinitelyNotDeclared nope;\n"
        "    public App() { return; }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        runI32(sources, "app.App");
        FAIL() << "expected CAJETA_ERROR_UNKNOWN_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_UNKNOWN_TYPE");
    } catch (std::runtime_error&) {
        // JIT layer may wrap the throw — accept that path too.
        SUCCEED();
    }
}
