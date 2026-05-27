//
// Operator-overload behavioral tests. v1 scope:
//   - Binary arithmetic / comparison operators on class types
//     declared as `public static` with two explicit parameters
//     (cajeta-docs/OperatorOverloading.md §2).
//   - Indexed access `[]` / `[]=` as instance methods.
//
// Deferred:
//   - Unary `++`, `--` (mutating, instance)
//   - Non-mutating unary `- + ! ~` (static, single operand)
//   - Compound assignment (`+= -= ...`) — derived from `+`/`-`/... by
//     default; explicit instance form overrides.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Most basic case: `a + b` where a is a class with a static
// operator+ defined dispatches to the operator method, not the
// built-in numeric add.
TEST(OperatorOverloadTests, classPlusDispatchesToStaticOperator) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public static int32 operator+ (Counter a, Counter b) { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter a = new Counter();\n"
        "        Counter b = new Counter();\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Equality operator: `a == b` for two class instances routes through
// the user-defined static operator==.
TEST(OperatorOverloadTests, classEqualsDispatchesToStaticOperator) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public static boolean operator== (Tag a, Tag b) { return true; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag a = new Tag();\n"
        "        Tag b = new Tag();\n"
        "        if (a == b) {\n"
        "            return 7;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Indexing operator: `obj[i]` for a class with `operator[]` defined
// dispatches to the operator method. The receiver is `this` (instance
// dispatch — indexed access stays instance per §5 of the spec), the
// index expression is the single named parameter, and the method's
// return value is the index expression's value.
TEST(OperatorOverloadTests, classIndexDispatchesToOperatorMethod) {
    // resolveMethod's canonical-name match is strict on parameter
    // type, so the index expression must literally be int64-typed —
    // an int32 literal `0` would mismatch the `operator[] (int64)`
    // declaration and fall through to the native-array path. Use an
    // explicit int64 local to make the lookup succeed.
    auto src =
        "package test;\n"
        "public class Lookup {\n"
        "    public int32 operator[] (int64 i) { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Lookup l = new Lookup();\n"
        "        int64 i = 0;\n"
        "        return l[i];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Write-form `obj[i] = v` dispatches to `operator[]=`. The class
// declares both `operator[]` (read) and `operator[]=` (write); each
// records into a tag field so the test can verify the write actually
// landed by reading back through `operator[]`.
TEST(OperatorOverloadTests, classIndexedAssignmentDispatchesToOperatorSet) {
    auto src =
        "package test;\n"
        "public class Cell {\n"
        "    public int32 tag;\n"
        "    public Cell() { this.tag = -1; }\n"
        "    public int32 operator[] (int64 i) { return this.tag; }\n"
        "    public void operator[]= (int64 i, int32 v) { this.tag = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell c = new Cell();\n"
        "        int64 i = 0;\n"
        "        c[i] = 77;\n"
        "        return c[i];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 77);
}

// Index argument flows through to the operator body. Doubles the
// passed index to prove the value reached the method correctly.
TEST(OperatorOverloadTests, classIndexPassesArgumentToOperatorMethod) {
    auto src =
        "package test;\n"
        "public class Doubler {\n"
        "    public int32 operator[] (int64 i) {\n"
        "        int64 v = i + i;\n"
        "        return (int32) v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Doubler d = new Doubler();\n"
        "        int64 i = 21;\n"
        "        return d[i];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Static binary `+` produces a fresh value at each step. Chained
// `a + b - c` walks through two static calls; the operands `a`,
// `b`, `c` survive unchanged, and `r` is a distinct fresh Vec.
// Multi-parameter free functions must return ownership (the borrow
// checker rejects a returned borrow because there's no privileged
// receiver whose lifetime would anchor it), so the operators
// declare `#Vec` and return `heap Vec(...)`.
TEST(OperatorOverloadTests, staticOperatorChainProducesFreshValues) {
    auto src =
        "package test;\n"
        "public class Vec {\n"
        "    public int32 v;\n"
        "    public Vec(int32 v) { this.v = v; }\n"
        "    public static #Vec operator+ (Vec a, Vec b) {\n"
        "        return heap Vec(a.v + b.v);\n"
        "    }\n"
        "    public static #Vec operator- (Vec a, Vec b) {\n"
        "        return heap Vec(a.v - b.v);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vec a = stack Vec(10);\n"
        "        Vec b = stack Vec(3);\n"
        "        Vec c = stack Vec(2);\n"
        "        Vec r = a + b - c;\n"
        "        return r.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// `#T operator+` — return ownership transfers to the caller. The
// operator body builds a fresh heap allocation and the caller
// receives an owning reference (drops at scope exit). Grammar
// accepts the `#` prefix on operator returns; visitor threads
// setReturnsOwnership(true) through.
TEST(OperatorOverloadTests, staticOperatorPlusCanReturnOwnershipTransfer) {
    auto src =
        "package test;\n"
        "public class Vec {\n"
        "    public int32 v;\n"
        "    public Vec(int32 v) { this.v = v; }\n"
        "    public static #Vec operator+ (Vec a, Vec b) {\n"
        "        return heap Vec(a.v + b.v);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vec a = stack Vec(3);\n"
        "        Vec b = stack Vec(4);\n"
        "        Vec c = a + b;\n"
        "        return c.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Instance `operator++` mutates `this`. Both x++ and ++x lower to
// `x.operator++()` for class-typed operands — the pre/post distinction
// is moot because the expression value is the receiver pointer either
// way, post-mutation. Borrow checker requires a mutable borrow at the
// call site.
TEST(OperatorOverloadTests, instanceIncrementMutatesReceiver) {
    auto src =
        "package test;\n"
        "public class Tick {\n"
        "    public int32 count;\n"
        "    public Tick() { this.count = 0; }\n"
        "    public void operator++ () {\n"
        "        this.count = this.count + 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tick t = new Tick();\n"
        "        t++;\n"
        "        t++;\n"
        "        t++;\n"
        "        return t.count;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// Static `operator-` for unary negation. Single-operand static
// method, takes the value, returns a fresh negated copy. The
// borrow checker requires `#T` + heap-return because a multi-param
// free function can't return a borrow — but a single-param static
// CAN return a borrow (the result's lifetime is bounded by the
// operand's), so `T operator-(T v)` with `stack T(...)` works.
TEST(OperatorOverloadTests, staticUnaryNegationDispatches) {
    auto src =
        "package test;\n"
        "public class Vec {\n"
        "    public int32 v;\n"
        "    public Vec(int32 v) { this.v = v; }\n"
        "    public static #Vec operator- (Vec a) {\n"
        "        return heap Vec(-a.v);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vec a = stack Vec(42);\n"
        "        Vec neg = -a;\n"
        "        return neg.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -42);
}

// Compound assignment derives from the static binary operator. The
// class declares `operator+` only; the compiler rewrites `a += b`
// to `a = T.operator+(a, b)`. No explicit `operator+=` needed.
TEST(OperatorOverloadTests, compoundAssignmentDerivesFromBinary) {
    auto src =
        "package test;\n"
        "public class Acc {\n"
        "    public int32 v;\n"
        "    public Acc(int32 v) { this.v = v; }\n"
        "    public static #Acc operator+ (Acc a, Acc b) {\n"
        "        return heap Acc(a.v + b.v);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Acc a = heap Acc(1);\n"
        "        Acc b = heap Acc(10);\n"
        "        a += b;\n"
        "        return a.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Visitor enforcement: `operator++` declared as static (wrong
// category) is rejected at parse time with
// CAJETA_ERROR_OPERATOR_NOT_INSTANCE.
TEST(OperatorOverloadTests, staticIncrementOperatorIsRejected) {
    auto src =
        "package test;\n"
        "public class Tick {\n"
        "    public int32 v;\n"
        "    public Tick() { this.v = 0; }\n"
        "    public static void operator++ (Tick t) {\n"  // wrong: must be instance
        "        t.v = t.v + 1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    bool threw = false;
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    } catch (...) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// != derivation: `a != b` on a class with operator== (but no
// operator!=) auto-derives as `!(a == b)`. Spec §7.
TEST(OperatorOverloadTests, notEqualsDerivesFromEquals) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 v;\n"
        "    public Tag(int32 v) { this.v = v; }\n"
        "    public static boolean operator== (Tag a, Tag b) {\n"
        "        return a.v == b.v;\n"
        "    }\n"
        "    public int64 hash() override { return (int64) this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag a = stack Tag(1);\n"
        "        Tag b = stack Tag(2);\n"
        "        Tag c = stack Tag(1);\n"
        "        if (a != b && !(a != c)) {\n"
        "            return 42;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// > / >= / <= derive from < (assuming total order):
//   a > b  ≡ b < a
//   a >= b ≡ !(a < b)
//   a <= b ≡ !(b < a)
TEST(OperatorOverloadTests, comparisonsDeriveFromLessThan) {
    auto src =
        "package test;\n"
        "public class V {\n"
        "    public int32 v;\n"
        "    public V(int32 v) { this.v = v; }\n"
        "    public static boolean operator< (V a, V b) {\n"
        "        return a.v < b.v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        V a = stack V(1);\n"
        "        V b = stack V(2);\n"
        "        // Tally a 1 for each comparison that returns the\n"
        "        // expected boolean; final score should be 7 (all four\n"
        "        // comparisons + 3 negations).\n"
        "        int32 score = 0;\n"
        "        if (a <  b) score = score + 1;\n"
        "        if (b >  a) score = score + 2;\n"
        "        if (a <= b) score = score + 4;\n"
        "        if (b >= a) score = score + 8;\n"
        "        if (!(b < a)) score = score + 16;\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    // 1 + 2 + 4 + 8 + 16 = 31
    EXPECT_EQ(runI32(src), 31);
}

// `operator!=` declared without `operator==` is rejected at parse
// time. != is auto-derived from == anyway; standalone != is almost
// always a bug (forgot to define == too).
TEST(OperatorOverloadTests, operatorNeWithoutEqIsRejected) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 v;\n"
        "    public Tag(int32 v) { this.v = v; }\n"
        "    public static boolean operator!= (Tag a, Tag b) {\n"
        "        return a.v != b.v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    bool threw = false;
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    } catch (...) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

// An instance-method binary `operator+` is rejected at parse time
// with CAJETA_ERROR_OPERATOR_NOT_STATIC. The user is steered toward
// the static form via the diagnostic.
TEST(OperatorOverloadTests, instanceBinaryOperatorIsRejected) {
    auto src =
        "package test;\n"
        "public class Vec {\n"
        "    public int32 v;\n"
        "    public Vec(int32 v) { this.v = v; }\n"
        "    public Vec operator+ (Vec other) {\n"      // missing `static`
        "        this.v = this.v + other.v;\n"
        "        return this;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    bool threw = false;
    try {
        auto jit = CajetaJit::compile(src, "test.D");
        (void) jit;
    } catch (...) {
        // cajeta::Exception doesn't inherit from std::exception, so a
        // generic catch is the portable way to confirm the parse
        // rejected the instance-method binary operator.
        threw = true;
    }
    EXPECT_TRUE(threw);
}
