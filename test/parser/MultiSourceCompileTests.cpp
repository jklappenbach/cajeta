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
//
// Parse-order limitation: the multi-source overload iterates the
// `sources` map in key order. If file A references a type defined
// in file B, B's class must register before A parses. Tests pick
// keys so dependencies sort alphabetically first. A topological
// pre-pass is a follow-up.
//
// Cross-module codegen-time call limitation: a USER constructor
// called across module boundaries (`new Counter()` in file A
// where Counter is defined in file B) currently trips the LLVM
// verifier after the post-codegen module-merge step — references
// inserted into A's IR aren't promoted to extern declarations,
// so the merge produces a function whose definition gets clobbered
// by a same-name forward reference. Cross-module SYNTHESIZED
// inject-method calls (the canonical DI shape) DO work, since
// they route through a different dispatch path. Promoting all
// cross-module method references to proper extern decls in the
// calling module is a codegen change tracked as a follow-up;
// the multi-source overload is meanwhile useful for the cross-
// module-DI shape the spec actually demands.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
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
// for tests that only inspect post-parse state (import maps,
// canonicalMap, structure-to-module registry) and don't need IR.
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
// that imports Dep and @Injects it. resolveDependencyGraph walks
// the process-global structureToModule map and threads the
// dependency across module boundaries. The synthesized inject
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

// Import-aware short-name resolution, inspected post-parse. Two
// classes named Logger live in different packages with different
// short names; a consumer file imports one. Verify that the
// consumer module's `imports` map captured the right qualified
// name and that fromContext on a bare `Logger` reference would
// resolve through it (we observe the captured import directly
// rather than re-running fromContext, since the consumer source
// here has no actual `Logger` use site).
TEST(MultiSourceCompileTests, importMapCapturesQualifier) {
    std::map<std::string, std::string> sources;
    sources["fast.Logger"] =
        "package fast;\n"
        "public class Logger { public Logger() { return; } }\n";
    sources["slow.Logger"] =
        "package slow;\n"
        "public class Logger { public Logger() { return; } }\n";
    sources["zz_app.App"] =
        "package zz_app;\n"
        "import slow.Logger;\n"
        "public final class App { public App() { return; } }\n";
    Compiler compiler;
    compileMultiForInspection(compiler, sources);

    // Find App's module and check its imports map.
    auto& s2m = CajetaModule::getStructureToModule();
    auto it = s2m.find("zz_app.App");
    ASSERT_NE(it, s2m.end()) << "zz_app.App should be in the registry";
    auto appModule = it->second;
    ASSERT_NE(appModule, nullptr);
    auto& imports = appModule->getImports();
    auto importIt = imports.find("Logger");
    ASSERT_NE(importIt, imports.end()) << "App should have an import for 'Logger'";
    ASSERT_FALSE(importIt->second.empty());
    auto& packageMap = importIt->second;
    auto pkgIt = packageMap.find("slow");
    ASSERT_NE(pkgIt, packageMap.end()) << "App's Logger import should resolve to package 'slow'";
    EXPECT_EQ(pkgIt->second->getTypeName(), "Logger");
    EXPECT_EQ(pkgIt->second->getPackageName(), "slow");
}
