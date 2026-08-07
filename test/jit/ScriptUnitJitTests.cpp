//
// script-units U2 (spec 2.2, 2.3, 3.1-3.4) — implicit class + entry synthesis.
//
// A script-shaped compilation unit compiles as an implicit final class with a
// synthetic static entry (`__cajeta_script_entry`): loose statements form the
// entry body in source order, top-level methods become static members, type
// declarations ride through as ordinary siblings. The unit's name derives
// from its source file stem; a package declaration is honored and its absence
// means the reserved `cajeta.script` package.
//
// Execution tests drive the JIT harness; placement/marker tests inspect the
// compiled module's structures (no JIT), the AnnotationParsingTests idiom.
// Session bindings (values that OUTLIVE the entry) are U3 — everything here
// observes execution through return values only.
//

#include "gtest/gtest.h"
#include "JitTestHelper.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;
using cajeta_test::CajetaJit;

namespace {

// Compile a source at the path implied by fqName under a fresh temp root and
// return the module for structure inspection (no JIT bring-up).
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_script_u2_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqName.size(); ++i) {
        if (i == fqName.size() || fqName[i] == '.') {
            rel /= fqName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    { std::ofstream out(full); out << source; }
    auto archive = base / "out";
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

}  // namespace

// 2.1.1 / spec 2.2 — loose statements execute in source order as the entry
// body; an explicit `return <int32>` ends the unit with that value (2.2.2).
TEST(ScriptUnitJitTests, statementsRun) {
    auto jit = CajetaJit::compile(
        "int32 a = 20;\n"
        "int32 b = 22;\n"
        "return a + b;\n",
        "cajeta.script.tool");
    ASSERT_NE(nullptr, jit);
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(42, entry());
}

// 2.2.2 — without an explicit return the entry returns 0.
TEST(ScriptUnitJitTests, defaultReturnIsZero) {
    auto jit = CajetaJit::compile(
        "int32 a = 1;\n"
        "a = a + 1;\n",
        "cajeta.script.tool");
    ASSERT_NE(nullptr, jit);
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(0, entry());
}

// 2.1.2 / spec 2.3 — a top-level method declared AFTER its use resolves
// (hoisting): methods are members of the implicit class, not statements.
TEST(ScriptUnitJitTests, topLevelMethodCallable) {
    auto jit = CajetaJit::compile(
        "int32 r = twice(21);\n"
        "return r;\n"
        "int32 twice(int32 v) { return v * 2; }\n",
        "cajeta.script.tool");
    ASSERT_NE(nullptr, jit);
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(42, entry());
}

// 2.1.3 / spec 2.2, 3.3 — a class declared in the unit is a normal top-level
// type the statements can instantiate.
TEST(ScriptUnitJitTests, declaredTypeUsable) {
    auto jit = CajetaJit::compile(
        "public class Point {\n"
        "    public int32 x;\n"
        "    public Point(int32 x) { this.x = x; }\n"
        "}\n"
        "Point p = heap Point(21);\n"
        "return p.x * 2;\n",
        "cajeta.script.tool");
    ASSERT_NE(nullptr, jit);
    auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(42, entry());
}

// 2.1.4 / spec 3.2, 2.5 — no package ⇒ the implicit class registers in the
// reserved `cajeta.script` package under the source stem; an explicit
// package declaration is honored.
TEST(ScriptUnitJitTests, reservedPackagePlacement) {
    Compiler compiler;
    auto module = compileForInspection(compiler,
        "int32 a = 1;\n", "cajeta.script.tool");
    ASSERT_NE(nullptr, module);
    auto& structures = module->getStructures();
    EXPECT_NE(structures.end(), structures.find("cajeta.script.tool"));

    Compiler compiler2;
    auto module2 = compileForInspection(compiler2,
        "package tools.demo;\n"
        "int32 a = 1;\n", "tools.demo.tool");
    ASSERT_NE(nullptr, module2);
    auto& structures2 = module2->getStructures();
    EXPECT_NE(structures2.end(), structures2.find("tools.demo.tool"));
}

// 2.1.5 / spec 3.4 — the implicit class carries the script-synthesized
// marker; a user-declared class in the same unit does not.
TEST(ScriptUnitJitTests, syntheticMarkerVisible) {
    Compiler compiler;
    auto module = compileForInspection(compiler,
        "public class Held { public Held() { return; } }\n"
        "int32 a = 1;\n",
        "cajeta.script.tool");
    ASSERT_NE(nullptr, module);
    auto& structures = module->getStructures();
    auto it = structures.find("cajeta.script.tool");
    ASSERT_NE(structures.end(), it);
    auto wrapper = std::dynamic_pointer_cast<cajeta::CajetaClass>(it->second);
    ASSERT_NE(nullptr, wrapper);
    EXPECT_TRUE(wrapper->isScriptSynthesized());

    auto user = structures.find("cajeta.script.Held");
    ASSERT_NE(structures.end(), user);
    auto userClass = std::dynamic_pointer_cast<cajeta::CajetaClass>(user->second);
    ASSERT_NE(nullptr, userClass);
    EXPECT_FALSE(userClass->isScriptSynthesized());
}
