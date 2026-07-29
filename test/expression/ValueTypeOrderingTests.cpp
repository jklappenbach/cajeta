//
// Value types get a DEFAULT field-wise ordering: a record with all-primitive
// fields synthesizes a lexicographic `operator<` (first differing field
// decides), so `<`, `>`, `<=`, `>=` work — and the type is usable where an
// ordering is required (e.g. a sortable collection). `>`/`<=`/`>=` derive from
// `<` via OperatorDispatch; `==`/`!=` from the existing equality synthesis.
// Part of the value-type-in-collections work (collection-literals).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t run(const std::string& pre, const std::string& body) {
    auto jit = CajetaJit::compile(pre +
        "public final class D { public static int32 run() {\n" + body + "} }\n",
        "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
const char* kPoint = "package test;\npublic record Point { int32 x; int32 y; }\n";
} // namespace

// Lexicographic `<`: the first differing field decides; `>` derives from `<`.
TEST(ValueTypeOrderingTests, lexicographicLessAndDerived) {
    EXPECT_EQ(run(kPoint,
        "  int32 r = 0;\n"
        "  if (Point{x:1,y:5} < Point{x:1,y:9}) { r = r + 1; }\n"     // +1 (y decides)
        "  if (Point{x:2,y:0} < Point{x:1,y:9}) { r = r + 10; }\n"    // no (x decides)
        "  if (Point{x:1,y:5} < Point{x:1,y:5}) { r = r + 100; }\n"   // no (equal)
        "  if (Point{x:3,y:0} > Point{x:1,y:9}) { r = r + 1000; }\n"  // +1000 (derived >)
        "  if (Point{x:1,y:5} <= Point{x:1,y:5}) { r = r + 10000; }\n"// +10000 (derived <=)
        "  return r;\n"), 11001);
}

// A user-declared `operator<` wins over the synthesized default.
TEST(ValueTypeOrderingTests, userOperatorLessWins) {
    EXPECT_EQ(run(
        "package test;\n"
        "public record Rev {\n"
        "    int32 v;\n"
        "    public static boolean operator< (Rev a, Rev b) { return a.v > b.v; }\n"  // reversed
        "}\n",
        "  int32 r = 0;\n"
        "  if (Rev{v:1} < Rev{v:9}) { r = r + 1; }\n"   // reversed: 1<9 is FALSE
        "  if (Rev{v:9} < Rev{v:1}) { r = r + 10; }\n"  // reversed: TRUE
        "  return r;\n"), 10);
}

// A float-field record orders too (the field `<` lowers to FCmp).
TEST(ValueTypeOrderingTests, floatFieldRecordOrders) {
    EXPECT_EQ(run(
        "package test;\npublic record F { float64 a; float64 b; }\n",
        "  int32 r = 0;\n"
        "  if (F{a:1.0, b:2.0} < F{a:1.0, b:3.0}) { r = 7; }\n"
        "  return r;\n"), 7);
}
