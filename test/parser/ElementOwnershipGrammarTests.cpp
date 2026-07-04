//
// element-ownership Unit 1 — grammar: `#` on type arguments and type
// parameters (spec §1.3, §8.4; plan 1.1).
//
// These are AST-level parser tests (no JIT): compile via Compiler and
// inspect module->getStructures(). They assert that:
//   - `#` on a type-parameter declaration (`class Cache<#K, V>`) sets the
//     owning-required flag on that parameter (declaration-`#`, §4.1.5), and a
//     plain parameter does not.
//   - `#` on a type argument (`HashMap<#String, V>`), including nested
//     positions, parses (the *semantic* checks are later units); the class
//     is produced, i.e. the grammar accepted the sigil.
//
// Test-first: the `owningRequired` accessor is a seam added default-false, so
// these compile and go RED until the grammar/visitor sets the flag.
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_eo_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_eo_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

} // namespace

// 1.1.2 — declaration-`#`: `class Cache<#K, V>` marks K owning-required, V not.
TEST(ElementOwnershipGrammarTests, declarationHashMarksOwningRequired) {
    auto src =
        "package test;\n"
        "public class Cache<#K, V> {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Cache");
    auto klass = module->getStructures()["test.Cache"];
    ASSERT_NE(klass, nullptr);
    const auto& params = klass->getTypeParameters();
    ASSERT_EQ(params.size(), 2u);
    EXPECT_TRUE(params[0].owningRequired);   // #K
    EXPECT_FALSE(params[1].owningRequired);  // V
}

// 1.1.2 — plain parameters carry no owning-required flag (dual-mode default).
TEST(ElementOwnershipGrammarTests, plainDeclarationNotOwningRequired) {
    auto src =
        "package test;\n"
        "public class HashMap<K, V> {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.HashMap");
    auto klass = module->getStructures()["test.HashMap"];
    ASSERT_NE(klass, nullptr);
    const auto& params = klass->getTypeParameters();
    ASSERT_EQ(params.size(), 2u);
    EXPECT_FALSE(params[0].owningRequired);
    EXPECT_FALSE(params[1].owningRequired);
}

// 1.1.1 — `#` on a type argument parses: a field typed `Box<#String>` produces
// the class (the grammar accepted the sigil). Semantic gating is a later unit.
TEST(ElementOwnershipGrammarTests, typeArgumentHashParses) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T value;\n"
        "}\n"
        "public class Holder {\n"
        "    public Box<#String> owned;\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Holder");
    auto klass = module->getStructures()["test.Holder"];
    ASSERT_NE(klass, nullptr);  // parsed: `#` accepted in type-argument position
}

// 1.1.3 — nested `#` positions parse: `Box<#Box<#String>>`.
TEST(ElementOwnershipGrammarTests, nestedTypeArgumentHashParses) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T value;\n"
        "}\n"
        "public class Holder {\n"
        "    public Box<#Box<#String>> nested;\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Holder");
    auto klass = module->getStructures()["test.Holder"];
    ASSERT_NE(klass, nullptr);
}
