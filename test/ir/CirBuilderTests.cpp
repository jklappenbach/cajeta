//
// Unit 2 (cajeta-ir plan) — AST -> CIR lowering for the closed slice.
// Compiles a small comparator-sort-shaped program, pulls the resolved
// Methods, lowers them with CirBuilder, and asserts the CIR shape:
//   - the generic callee (`each`) body has apply.closure %cmp(...) sites;
//   - the wrapper (`run`) has a make.closure(<fn>, []) with target-known;
//   - a stored/runtime closure (`runStored`, closure passed by name) lowers
//     WITHOUT a make.closure (not a directly-supplied lambda -> not known here).
// Traces spec §2, §3.1.1.
//
// Uses concrete int32 element types (not `<T>`): a generic *template* method
// keeps its body as re-parsed source (no parsed Block until instantiated), so
// the closed-slice lowering is validated on concrete-typed methods. The builder
// itself stays generic-ready (it reads methodTypeParameters).
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "cajeta/ir/CirBuilder.h"
#include "cajeta/ir/CirPrinter.h"
#include "cajeta/ir/CirVerifier.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::ir::CirBuilder;
using cajeta::ir::CirPrinter;
using cajeta::ir::CirVerifier;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_cir_" + std::to_string(rng()));
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
                 / ("cajeta_cir_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass, const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// A comparator-sort-shaped slice: a generic-callee-shaped `each` that invokes a
// function-typed parameter, a `run` wrapper that passes a directly-supplied
// non-capturing lambda, and a `runStored` that forwards a stored closure.
const char* kSource =
    "package test;\n"
    "public class S {\n"
    "    public static void each(int32[] a, int32 n, (int32, int32) -> int32 cmp) {\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            if (cmp(a[i], a[i]) > 0) { i = i + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "    }\n"
    "    public static void run(int32[] a, int32 n) {\n"
    "        each(a, n, (x, y) -> {\n"
    "            if (x < y) { return -1; }\n"
    "            if (x > y) { return 1; }\n"
    "            return 0;\n"
    "        });\n"
    "    }\n"
    "    public static void runStored(int32[] a, int32 n, (int32, int32) -> int32 c) {\n"
    "        each(a, n, c);\n"
    "    }\n"
    "}\n";

} // namespace

// 2.1.1 — the generic callee body has apply.closure %cmp(...) sites.
TEST(CirBuilderTests, calleeBodyHasApplyClosure) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.S");
    auto klass = module->getStructures()["test.S"];
    ASSERT_NE(klass, nullptr);
    auto each = findMethod(klass, "each");
    ASSERT_NE(each, nullptr);

    auto cir = CirBuilder::buildFunction(each);
    ASSERT_NE(cir, nullptr);
    std::string text = CirPrinter::print(*cir);

    EXPECT_TRUE(has(text, "fn each(")) << text;
    EXPECT_TRUE(has(text, "apply.closure %cmp(")) << text;
    // The closure operand is the function-typed parameter (no defining
    // make.closure in this function -> knownness resolved at the call boundary).
    EXPECT_FALSE(has(text, "make.closure")) << text;
    EXPECT_TRUE(CirVerifier::verify(*cir).empty());
}

// 2.1.1 — the wrapper has make.closure(<fn>, []) with target-known for the
// directly-supplied non-capturing lambda.
TEST(CirBuilderTests, wrapperHasKnownMakeClosure) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.S");
    auto klass = module->getStructures()["test.S"];
    ASSERT_NE(klass, nullptr);
    auto run = findMethod(klass, "run");
    ASSERT_NE(run, nullptr);

    auto cir = CirBuilder::buildFunction(run);
    ASSERT_NE(cir, nullptr);
    std::string text = CirPrinter::print(*cir);

    EXPECT_TRUE(has(text, "make.closure")) << text;
    EXPECT_TRUE(has(text, "[]")) << text;        // non-capturing: empty captures
    EXPECT_TRUE(has(text, "known")) << text;     // §2.4.1 statically-known target
    EXPECT_TRUE(CirVerifier::verify(*cir).empty());
}

// 2.1.2 — a stored/runtime closure (passed by name, not a lambda literal) lowers
// WITHOUT a make.closure: it is not a directly-supplied lambda, so its target is
// not known at this site (the Phase-A classifier leaves it indirect).
TEST(CirBuilderTests, storedClosureHasNoKnownMakeClosure) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.S");
    auto klass = module->getStructures()["test.S"];
    ASSERT_NE(klass, nullptr);
    auto runStored = findMethod(klass, "runStored");
    ASSERT_NE(runStored, nullptr);

    auto cir = CirBuilder::buildFunction(runStored);
    ASSERT_NE(cir, nullptr);
    std::string text = CirPrinter::print(*cir);

    EXPECT_FALSE(has(text, "make.closure")) << text;   // closure is a stored param
    EXPECT_TRUE(has(text, "call each(")) << text;      // direct call, closure passed by value
    EXPECT_TRUE(has(text, "%c")) << text;
    EXPECT_TRUE(CirVerifier::verify(*cir).empty());
}

// Out-of-slice safety: a null method yields null; a trivial method yields a
// well-formed single-block CIR (no crash, no false "known").
TEST(CirBuilderTests, trivialAndNullAreSafe) {
    EXPECT_EQ(CirBuilder::buildFunction(nullptr), nullptr);

    auto src =
        "package test;\n"
        "public class T {\n"
        "    public static void empty() { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.T");
    auto klass = module->getStructures()["test.T"];
    ASSERT_NE(klass, nullptr);
    auto m = findMethod(klass, "empty");
    ASSERT_NE(m, nullptr);

    auto cir = CirBuilder::buildFunction(m);
    ASSERT_NE(cir, nullptr);
    EXPECT_TRUE(CirVerifier::verify(*cir).empty());
    std::string text = CirPrinter::print(*cir);
    EXPECT_TRUE(has(text, "fn empty()")) << text;
    EXPECT_FALSE(has(text, "make.closure")) << text;
}
