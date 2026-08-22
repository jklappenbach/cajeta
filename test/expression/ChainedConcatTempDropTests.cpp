//
// Interior temporaries of a CHAINED String concat — element-ownership 3.4.3,
// the sibling of CallArgTempDropTests.
//
// `"a" + i + "b"` parses as `(("a" + i) + "b")`. Only the OUTER node is ever
// given an owner: the arena pre-pass (Method.cpp arenaConcatInit) collects the
// top-level BinaryOpExpression of a declarator initializer and marks just that
// node arena-eligible, and a declarator's drop entry covers only the value the
// name binds. The interior `("a" + i)` node is a separate BinaryOpExpression
// that mallocs a wrapper, has its bytes copied into the parent result, and is
// then never freed — one leaked wrapper per interior `+`, plus its heap byte
// buffer when the interior text exceeds the 12-byte inline capacity.
//
// Measured before the fix: `"x" + i + "z"` leaked 1 object per evaluation and
// `"x" + i + "z" + i` leaked 2; splitting the same expression across two
// statements leaked none. Operand type is irrelevant — an all-String chain
// leaks identically. Found via cajeta-llama's bind path
// (`"model.layers." + l + "."`), which leaked 4 objects per model bind.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Live-object delta across an inner scope containing `body`
// (the CallArgTempDropTests harness shape).
int64_t liveDelta(const std::string& body) {
    std::string src =
        "package test;\n"
        "public class Root {\n"
        "    public String v;\n"
        "    public Root() { return; }\n"
        "    public void ignore(String s) { return; }\n"
        "}\n"
        "public final class D {\n"
        "    static #String make(int32 i) { return \"x\" + i + \"z\"; }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        if (true) {\n"
        "            " + body + "\n"
        "        }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int64_t (*)()>("run")();
}

} // namespace

// The defect in one line: two `+` in one expression, one interior temp.
TEST(ChainedConcatTempDropTests, interiorTempOfThreeOperandConcatIsDropped) {
    EXPECT_EQ(liveDelta("int32 i = 7;\nString s = \"x\" + i + \"z\";"), 0);
}

// Each additional `+` adds an interior node; all of them must be reclaimed.
TEST(ChainedConcatTempDropTests, everyInteriorTempOfALongChainIsDropped) {
    EXPECT_EQ(liveDelta(
        "int32 i = 7;\nString s = \"x\" + i + \"z\" + i + \"w\";"), 0);
}

// Operand type is irrelevant — an all-String chain has the same interior node.
TEST(ChainedConcatTempDropTests, allStringChainIsDropped) {
    EXPECT_EQ(liveDelta(
        "String a = \"aa\";\nString s = a + \"bb\" + \"cc\";"), 0);
}

// An interior result longer than the 12-byte inline capacity also owns a heap
// byte buffer; both the wrapper and the buffer must go.
TEST(ChainedConcatTempDropTests, longInteriorTempFreesItsHeapBuffer) {
    EXPECT_EQ(liveDelta(
        "int32 i = 7;\n"
        "String s = \"model.layers.\" + i + \".self_attn.q_proj.weight\";"), 0);
}

// The chain as a CALL ARGUMENT: the outer temp is handled by the call-arg
// drop (CallArgTempDropTests); the interior one is this defect.
TEST(ChainedConcatTempDropTests, chainAsCallArgumentLeavesNoTemp) {
    EXPECT_EQ(liveDelta(
        "Root d = stack Root();\n"
        "int32 i = 7;\n"
        "d.ignore(\"x\" + i + \"z\");"), 0);
}

// Splitting the same expression across statements was always clean — it stays
// clean (the fix must not double-free a named intermediate).
TEST(ChainedConcatTempDropTests, splitAcrossStatementsStaysBalanced) {
    EXPECT_EQ(liveDelta(
        "int32 i = 7;\nString t = \"x\" + i;\nString s = t + \"z\";"), 0);
}

// A chain that ESCAPES by return still yields a live, correct value to the
// caller: the fix must reclaim interior nodes only, never the result.
TEST(ChainedConcatTempDropTests, returnedChainSurvivesAndReadsBack) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    static #String make(int32 i) { return \"x\" + i + \"z\"; }\n"
        "    public static int32 run() {\n"
        "        String s #= D.make(7);\n"
        "        return (int32) s.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(3, jit->lookup<int32_t (*)()>("run")());
}

// And the text itself is unchanged by the reclamation.
TEST(ChainedConcatTempDropTests, chainedConcatTextIsCorrect) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 i = 42;\n"
        "        String s = \"model.layers.\" + i + \".weight\";\n"
        "        String want = \"model.layers.42.weight\";\n"
        "        return s.equals(want) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(1, jit->lookup<int32_t (*)()>("run")());
}
