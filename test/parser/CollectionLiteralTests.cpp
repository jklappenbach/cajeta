//
// collection-literals Unit 1 — target-typed collection literals (list/set).
// A bare `[...]` literal against a collection-class target (e.g.
// `ArrayList<int32> xs = [1,2,3]`) lowers to a from-array constructor call
// `heap Target([...])`, reusing the array-literals target-typing machinery and
// the `(T[])` constructors. The literal's element type is taken from the
// target's type argument (so `ArrayList<int64> = [1,2,3]` widens). An array
// target keeps the array path; a class without a `(T[])` ctor errors cleanly.
// (spec §2; plan Unit 1).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n" + imports +
        "public final class D {\n" + body + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
} // namespace

// 1.1.1 — ArrayList from a bare literal: size 3, order preserved, heap default.
TEST(CollectionLiteralTests, ArrayListFromBareLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = [1, 2, 3];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 1*10 + 3 = 313
    EXPECT_EQ(r, 313);
}

// 1.1.2 — HashSet from a bare literal: duplicates collapse to {1,2,3}.
TEST(CollectionLiteralTests, HashSetFromBareLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.HashSet;\n",
        "    public static int32 run() {\n"
        "        HashSet<int32> s = [1, 2, 2, 3];\n"
        "        int32 c = (int32) s.count();\n"          // 3 (deduped)
        "        int32 has = 0;\n"
        "        if (s.contains(1)) { has = has + 1; }\n"
        "        if (s.contains(2)) { has = has + 1; }\n"
        "        if (s.contains(3)) { has = has + 1; }\n"
        "        if (s.contains(9)) { has = has + 10; }\n"  // absent
        "        return c * 10 + has;\n"                    // 3*10 + 3 = 33
        "    }\n");
    EXPECT_EQ(r, 33);
}

// 1.1.2b — the target's type argument drives the element type: an
// `ArrayList<int64>` from `[1,2,3]` builds an `int64[]` for the ctor (widening
// past the int32 the literal would unify to on its own). plan 1.2.2.
TEST(CollectionLiteralTests, ElementTypeFromTarget) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int64> xs = [1, 2, 3];\n"
        "        int64 sum = xs.get(0) + xs.get(1) + xs.get(2);\n"
        "        return (int32) sum;\n"                     // 6
        "    }\n");
    EXPECT_EQ(r, 6);
}

// 1.1.3 — a `stack`-prefixed collection literal still produces the right
// values; the prefix carries to the collection construction (the inner array
// stays a heap ctor argument). plan 1.2.1 ("carry a stack/shared prefix
// through").
TEST(CollectionLiteralTests, StackCollectionLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = stack [1, 2, 3];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 1*10 + 3 = 313
    EXPECT_EQ(r, 313);
}

// 1.1.4 — an array target is unchanged: `[...]` against `int32[]` stays an
// array (the class-target polymorphism doesn't hijack the array path).
TEST(CollectionLiteralTests, ArrayTargetStillArray) {
    int32_t r = runI32(
        "",
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return xs[0] * 100 + xs[1] * 10 + xs[2];\n"
        "    }\n");                                    // 123
    EXPECT_EQ(r, 123);
}

// 1.1.4b — assignment (not just declaration) also target-types a collection:
// `xs = [...]` on an already-declared ArrayList local.
TEST(CollectionLiteralTests, AssignmentTargetsCollection) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = [1];\n"
        "        xs = [4, 5, 6];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 4*10 + 6 = 346
    EXPECT_EQ(r, 346);
}

// 1.1.5 — a class target without a `(T[])` constructor gives a compile error,
// not a silent miss. `Box<int32>` has only a `(T)` ctor.
TEST(CollectionLiteralTests, NoCollectionCtorErrors) {
    std::string src =
        "package test;\n"
        "public final class Box<T> {\n"
        "    private T v;\n"
        "    public Box(T v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = [1, 2, 3];\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}
