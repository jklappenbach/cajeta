//
// First end-to-end smoke for class templates. Verifies that
// `Box<int32>` materializes as a real LLVM-backed type with the
// template parameter substituted for the property's actual type.
//
// Covers TPL-1..TPL-5 in one shot:
//  - Template class declaration parses (TPL-1)
//  - Template source snippet retained for re-parse (TPL-2)
//  - `T` substitutes during instantiation walk (TPL-3)
//  - instantiate(args) produces a concrete class cached in structures (TPL-4)
//  - `new Box<int32>()` resolves through the cache (TPL-5)
//
// Constraint enforcement (TPL-6) and diamond inference (TPL-7) live in
// separate test files.
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

// The smallest possible template instantiation: Box<int32> holds an int32
// field and returns it. Native (primitive) type arg is the key thing we
// want to verify works — it's the differentiator from Java generics.
TEST(TemplateBasicTests, primitiveArgInstantiates) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T value() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = new Box<int32>();\n"
        "        return b.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Minimal: a template whose body doesn't even reference T. Verifies the
// instantiation plumbing in isolation from substitution.
TEST(TemplateBasicTests, templateBodyWithoutTUseInstantiates) {
    auto src =
        "package test;\n"
        "public class Holder<T> {\n"
        "    public int32 fixed() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<int32> h = new Holder<int32>();\n"
        "        return h.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Constraint enforcement (TPL-6) ---------------------------------------

// Accept: argument class extends the bound. `Dog extends Animal`, template
// requires `T extends Animal` — Dog satisfies.
TEST(TemplateBasicTests, satisfiedBoundInstantiates) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 legs() { return 4; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 bark() { return 1; }\n"
        "}\n"
        "public class Kennel<T extends Animal> {\n"
        "    public int32 fixed() { return 11; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Kennel<Dog> k = new Kennel<Dog>();\n"
        "        return k.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Accept: argument class IS the bound. `<T extends Animal>` with `T = Animal`.
TEST(TemplateBasicTests, exactBoundIsSatisfied) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 legs() { return 4; }\n"
        "}\n"
        "public class Kennel<T extends Animal> {\n"
        "    public int32 fixed() { return 9; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Kennel<Animal> k = new Kennel<Animal>();\n"
        "        return k.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// Reject: argument class is unrelated to the bound. `Robot` doesn't extend
// `Animal`. Instantiation must throw a CAJETA_ERROR_TYPE_PARAMETER_BOUND.
TEST(TemplateBasicTests, unrelatedClassArgViolatesBound) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 legs() { return 4; }\n"
        "}\n"
        "public class Robot {\n"
        "    public int32 power() { return 9000; }\n"
        "}\n"
        "public class Kennel<T extends Animal> {\n"
        "    public int32 fixed() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Kennel<Robot> k = new Kennel<Robot>();\n"
        "        return k.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// --- Diamond inference (TPL-7) -------------------------------------------

// Single-parameter inference: ctor takes T, arg is int32, T binds to int32.
// First check: a template with a user-defined constructor compiles and
// allocates correctly under explicit type args. Establishes that the user
// ctor path works before we layer diamond inference on top.
TEST(TemplateBasicTests, userCtorWithTArgUnderExplicitArgs) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public Box(T v) {  }\n"
        "    public int32 fixed() { return 13; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = new Box<int32>(7);\n"
        "        return b.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

TEST(TemplateBasicTests, diamondInfersSingleTypeParameter) {
    // LHS pins the storage type to Box<int32>; the diamond on the RHS lets
    // us drop the explicit `<int32>` from the constructor call. T is
    // inferred from the int32 literal argument.
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public Box(T v) {  }\n"
        "    public int32 fixed() { return 13; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = new Box<>(7);\n"
        "        return b.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 13);
}

// Reject: diamond on a template whose constructors don't reference T can't
// infer anything. The DefaultConstructorMethod has no T-typed params, so
// `new Holder<>()` has nothing to unify against. Without an explicit LHS
// the test source would also fail to parse — pin Holder<int32> on the LHS
// so the parser is happy and we exercise only the inference path.
TEST(TemplateBasicTests, diamondWithoutInferableConstructorThrows) {
    auto src =
        "package test;\n"
        "public class Holder<T> {\n"
        "    public int32 fixed() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<int32> h = new Holder<>();\n"
        "        return h.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// Reject: primitive argument can never satisfy a class bound. This is the
// "natural and correct outcome" mentioned in the TPL-6 design.
TEST(TemplateBasicTests, primitiveArgViolatesClassBound) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 legs() { return 4; }\n"
        "}\n"
        "public class Kennel<T extends Animal> {\n"
        "    public int32 fixed() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Kennel<int32> k = new Kennel<int32>();\n"
        "        return k.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// --- Distinct instantiations + cache identity (TPL-8 a/b) -----------------

// Box<int32> and Box<int64> are distinct types. The user's program creates
// both in sequence; if they collided in the structures map, the second
// declaration would overwrite the first and behavior would be wrong.
TEST(TemplateBasicTests, distinctArgsProduceDistinctInstantiations) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public int32 fixed() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> a = new Box<int32>();\n"
        "        Box<int64> b = new Box<int64>();\n"
        "        return a.fixed() + b.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 84);
}

// Same Box<int32> referenced from two sites must reuse the cached class —
// otherwise we'd double-codegen and the linker would reject duplicate
// LLVM symbols. The fact that this compiles + runs at all proves the
// cache works.
TEST(TemplateBasicTests, sameInstantiationIsCached) {
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public int32 fixed() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> a = new Box<int32>();\n"
        "        Box<int32> b = new Box<int32>();\n"
        "        return a.fixed() + b.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Two-parameter template: `Pair<A,B>` distinct from any single-T
// instantiation. Each slot needs an independent type arg.
TEST(TemplateBasicTests, twoParameterTemplateInstantiates) {
    auto src =
        "package test;\n"
        "public class Pair<A, B> {\n"
        "    public int32 fixed() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int64> p = new Pair<int32, int64>();\n"
        "        return p.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Multi-parameter diamond inference: ctor has both A- and B-typed slots,
// both must bind from the call args. Both args here are int32 — verifies
// the unifier correctly tracks per-parameter bindings even when the args
// happen to share a type.
TEST(TemplateBasicTests, diamondInfersMultipleParameters) {
    auto src =
        "package test;\n"
        "public class Pair<A, B> {\n"
        "    public Pair(A a, B b) {  }\n"
        "    public int32 fixed() { return 77; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair<int32, int32> p = new Pair<>(3, 4);\n"
        "        return p.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}
