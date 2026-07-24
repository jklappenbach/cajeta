//
// nucleo-frame U1 (spike) — cross-type operator returns (plan 1.1.1).
//
// The typed DSL needs `col.price > 0.0` to build a PREDICATE node — a
// comparison returning a class other than boolean, and arithmetic returning
// a class other than the operands'. Both now work in EVERY position:
// comparison typing consults the operand class's operator override and takes
// its DECLARED return type (boolean only when no override resolves), and the
// pre-pass no longer stamps a premature boolean when operand types are not
// yet resolvable — the stamp was exactly what broke argument-position
// overload resolution (`take(a > 5.0)` missed `take(BoolExpr)`).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

TEST(DslOperatorSpikeTests, crossTypeOperatorReturnsEveryPosition) {
    std::string src =
        "package test;\n"
        "public class NumExpr {\n"
        "    int32 tag;\n"
        "    public NumExpr(int32 tag) { this.tag = tag; }\n"
        "    public int32 tag() { return this.tag; }\n"
        "    public static #PairExpr operator*(NumExpr a, NumExpr b) {\n"
        "        return heap PairExpr(a.tag() * 100 + b.tag());\n"
        "    }\n"
        "    public static #BoolExpr operator>(NumExpr a, float64 v) {\n"
        "        return heap BoolExpr(a.tag() * 10 + (int32) v);\n"
        "    }\n"
        "}\n"
        "public class PairExpr {\n"
        "    int32 code;\n"
        "    public PairExpr(int32 code) { this.code = code; }\n"
        "    public int32 code() { return this.code; }\n"
        "}\n"
        "public class BoolExpr {\n"
        "    int32 code;\n"
        "    public BoolExpr(int32 code) { this.code = code; }\n"
        "    public int32 code() { return this.code; }\n"
        "}\n"
        "public final class T {\n"
        "    static int32 take(BoolExpr e) { return e.code(); }\n"
        "    public static int32 run() {\n"
        "        NumExpr a = heap NumExpr(3);\n"
        "        NumExpr b = heap NumExpr(7);\n"
        "        PairExpr p = a * b;\n"              // arithmetic, assignment
        "        BoolExpr c = a > 5.0;\n"            // comparison, assignment
        "        int32 argPos = take(a > 5.0);\n"    // ARGUMENT position
        "        int32 chained = take(heap NumExpr(9) > 2.0);\n"  // temp recv
        "        boolean plain = 3 > 2;\n"           // primitives: unchanged
        "        int32 pb = 0;\n"
        "        if (plain) { pb = 1; }\n"
        "        return p.code() * 100000 + c.code() * 1000 + argPos * 10\n"
        "            + chained - 92 + pb;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // 307*100000 + 35*1000 + 35*10 + (92-92) + 1 = 30735351
    EXPECT_EQ(fn(), 30735351);
}
