//
// Comparing a STATIC field of interface type against null.
//
// THE CRASH: the interface-vs-null path in BinaryOpExpression only fired when
// the interface operand arrived as a POINTER to its fat struct. Locals and
// instance fields do arrive that way; a static field loads as the fat VALUE
// itself. So for statics the whole block was skipped and the struct reached
// CreateICmp against a null pointer, killing the compiler in LLVM:
//
//   ICmpInst::AssertOK(): Assertion `getOperand(0)->getType() ==
//   getOperand(1)->getType() && "Both operands to ICmp instruction are not of
//   the same type!"' failed.
//
// Found 2026-08-27 adding an opt-in diagnostics hook to cajeta-llm, whose
// whole design is "a static sink field, null when nobody is listening" — the
// single most obvious way to write that shape. The workaround (carry the armed
// state in a parallel boolean) is exactly the sort of thing that should not be
// necessary, hence this fix.
//
// The tests cover BOTH directions, because a fix that always reported "null"
// would satisfy a one-sided test while breaking every guard written this way:
// the field must read null before assignment and non-null after, through `==`
// and `!=` alike. The local/instance shapes are pinned too — they always
// worked, and a fix to the static path must not regress them.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
// Shared preamble: one interface and one implementation.
const char* kIface =
    "package test;\n"
    "public interface Sink { int64 tag(); }\n"
    "public final class Impl implements Sink {\n"
    "    public Impl() { return; }\n"
    "    public int64 tag() { return 7; }\n"
    "}\n";
}  // namespace

// The crash itself: `!=` against a static interface field. Compiling at all
// used to be impossible.
TEST(InterfaceStaticNullCompareTests, staticInterfaceFieldComparesToNull) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        if (D.held == null) { score = score + 1; }\n"   // 1
        "        if (D.held != null) { score = score + 100; }\n" // not taken
        "        D.held = heap Impl();\n"
        "        if (D.held != null) { score = score + 10; }\n"  // 11
        "        if (D.held == null) { score = score + 100; }\n" // not taken
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// Null on the LEFT — the operand-order arm of the same path.
TEST(InterfaceStaticNullCompareTests, nullOnTheLeftAlsoWorks) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        if (null == D.held) { score = score + 1; }\n"
        "        D.held = heap Impl();\n"
        "        if (null != D.held) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// A guarded dispatch through the static field — the actual shape the engine
// wanted: check, then call. Proves the non-null value is still usable, not
// merely comparable.
TEST(InterfaceStaticNullCompareTests, staticFieldGuardThenDispatch) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    static Sink held;\n"
        "    public static int64 run() {\n"
        "        int64 total = 0;\n"
        "        if (D.held != null) { total = total + D.held.tag(); }\n"
        "        D.held = heap Impl();\n"
        "        if (D.held != null) { total = total + D.held.tag(); }\n"
        "        return total;\n"                                  // 0 + 7
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Controls for the shapes that ALWAYS worked, pinned so the static fix cannot
// regress them. Split in two so a failure names the shape rather than the pair.
TEST(InterfaceStaticNullCompareTests, localInterfaceShapeStillCompares) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        Sink local = null;\n"
        "        if (local == null) { score = score + 1; }\n"
        "        Sink live #= heap Impl();\n"
        "        if (live != null) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

TEST(InterfaceStaticNullCompareTests, instanceFieldInterfaceShapeStillCompares) {
    auto jit = CajetaJit::compile(
        std::string(kIface) +
        "public final class Holder {\n"
        "    public Sink field;\n"
        "    public Holder() { this.field = null; }\n"
        "    public void set(#Sink s) { this.field #= s; return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 score = 0;\n"
        "        Holder h = heap Holder();\n"
        "        if (h.field == null) { score = score + 1; }\n"
        "        Sink live #= heap Impl();\n"
        "        h.set(#live);\n"
        "        if (h.field != null) { score = score + 10; }\n"
        "        return score;\n"
        "    }\n"
        "}\n", "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}
