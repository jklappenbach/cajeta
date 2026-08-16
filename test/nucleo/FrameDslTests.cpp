//
// nucleo-frame U1 (spike close) — the typed DSL node family (plan 1.1.3,
// 1.2.2): synthesized builders return typed column-reference nodes; operators
// compose introspectable predicate/arithmetic trees; a TYPE mismatch in the
// DSL is a COMPILE error (no such operator) — the §3.2 claim.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
const char* kPrefix =
    "package test;\n"
    "import cajeta.nucleo.frame.ColF64;\n"
    "import cajeta.nucleo.frame.ColStr;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "public record Tick {\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    public Tick(float64 price, float64 size) {\n"
    "        this.price #= price;\n"
    "        this.size #= size;\n"
    "    }\n"
    "}\n"
    "public class Table<T> {\n"
    "    T sample;\n"
    "    public Table(T sample) {\n"
    "        this.sample #= sample;\n"
    "    }\n"
    "}\n";
} // namespace

// 1.2.2 — the node family: builders yield colRefs; operators build
// introspectable trees (and-of-comparisons, arithmetic, string predicates).
TEST(FrameDslTests, typedBuildersComposeIntrospectableTrees) {
    std::string src = std::string(kPrefix) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        Table<Tick> ticks = heap Table<Tick>(stack Tick(1.0, 2.0));\n"
        "        TickCols c = heap TickCols();\n"
        "        Pred p = (c.price() > 1.5) & (c.size() < 10.0);\n"
        "        int32 acc = 0;\n"
        "        if (p.kind() == 30) { acc = acc + 1; }\n"
        "        Pred l = p.leftPredicate();\n"
        "        if (l.kind() == 20) { acc = acc + 10; }\n"
        "        if (l.operandExpr().ordinal() == 0) { acc = acc + 100; }\n"
        "        if (l.value() == 1.5) { acc = acc + 1000; }\n"
        "        Pred r = p.rightPredicate();\n"
        "        if (r.kind() == 22) { acc = acc + 10000; }\n"
        "        if (r.operandExpr().ordinal() == 1) { acc = acc + 100000; }\n"
        "        ColF64 vwapExpr = c.price() * c.size();\n"
        "        if (vwapExpr.kind() == 4) { acc = acc + 1000000; }\n"
        "        if (vwapExpr.leftChild().ordinal() == 0) { acc = acc + 10000000; }\n"
        "        ColStr sc #= ColStr.colRef(2, \"venue\");\n"
        "        Pred sp = sc == \"NYSE\";\n"
        "        if (sp.kind() == 26) { acc = acc + 100000000; }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 111111111);
}

// 1.1.3 — the decision test: a numeric comparison on a STRING column has no
// operator — a located compile error, never a runtime surprise.
TEST(FrameDslTests, typeMismatchInDslIsCompileError) {
    std::string src = std::string(kPrefix) +
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        ColStr sc #= ColStr.colRef(2, \"venue\");\n"
        "        Pred p = sc > 0.0;\n"
        "        return p.kind();\n"
        "    }\n"
        "}\n";
    bool threw = false;
    try {
        CajetaJit::compile(src, "test.T");
    } catch (cajeta::Exception& e) {
        threw = true;
        // The error names the missing operator/overload, not a crash.
        EXPECT_FALSE(std::string(e.getErrorId()).empty());
    }
    EXPECT_TRUE(threw);
}
