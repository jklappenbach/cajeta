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
//  - `heap Box<int32>()` resolves through the cache (TPL-5)
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
// want to verify works — it's the differentiator from Java's type-erased
// generics (and matches C#'s approach: real distinct types per arg list).
// Class-level type parameters referenced inside a regular method's
// signature — NOT a method-level template. The method is a plain
// methodDeclaration; the `T` in its parameter type and return type is
// bound by the surrounding `class MyClass<T> { ... }` and gets
// substituted at instantiation time. Method-level template syntax
// (`<U> U someFunc(U x)`) is explicitly absent from the grammar; see
// the matching note above `memberDeclaration` in CajetaParser.g4.
TEST(TemplateBasicTests, classTypeParameterFlowsIntoMethodSignature) {
    auto src =
        "package test;\n"
        "public class Pipe<T> {\n"
        "    public T through(T value) { return value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pipe<int32> p = heap Pipe<int32>();\n"
        "        return p.through(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}


// Minimal: a template whose body doesn't even reference T. Verifies the
// instantiation plumbing in isolation from substitution.

// --- Constraint enforcement (TPL-6) ---------------------------------------

// Accept: argument class extends the bound. `Dog extends Animal`, template
// requires `T extends Animal` — Dog satisfies.

// Accept: argument class IS the bound. `<T extends Animal>` with `T = Animal`.

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
        "        Kennel<Robot> k = heap Kennel<Robot>();\n"
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


// Reject: diamond on a template whose constructors don't reference T can't
// infer anything. The DefaultConstructorMethod has no T-typed params, so
// `heap Holder<>()` has nothing to unify against. Without an explicit LHS
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
        "        Holder<int32> h = heap Holder<>();\n"
        "        return h.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}

// Reject: primitive argument can never satisfy a class bound. This is the
// "natural and correct outcome" mentioned in the TPL-6 design.

// --- Distinct instantiations + cache identity (TPL-8 a/b) -----------------

// Box<int32> and Box<int64> are distinct types. The user's program creates
// both in sequence; if they collided in the structures map, the second
// declaration would overwrite the first and behavior would be wrong.

// Same Box<int32> referenced from two sites must reuse the cached class —
// otherwise we'd double-codegen and the linker would reject duplicate
// LLVM symbols. The fact that this compiles + runs at all proves the
// cache works.

// Two-parameter template: `Pair<A,B>` distinct from any single-T
// instantiation. Each slot needs an independent type arg.

// --- Nested templates (TPL-N1) -------------------------------------------

// Parameterized super: `class List<T> extends Container<T>`. Instantiating
// `List<int32>` must also instantiate `Container<int32>` with the same T
// binding, and dispatch should reach Container's inherited method through
// the vtable.

// Baseline for nested-arg inference: same template structure with explicit
// type args. Verifies that a templated-class-typed constructor arg works at
// all before we layer inference on top.

// Diamond inference where the ctor parameter type is itself parameterized:
// `Wrapper<T>(List<T> v)`. From a `List<int32>` arg, T should bind to int32.
// The unifier has to recurse into nested template arguments to recover the
// binding from the arg's own typeArguments.
TEST(TemplateBasicTests, diamondInfersThroughNestedParameter) {
    auto src =
        "package test;\n"
        "public class List<T> {\n"
        "    public int32 listMark() { return 1; }\n"
        "}\n"
        "public class Wrapper<T> {\n"
        "    public Wrapper(List<T> v) {  }\n"
        "    public int32 fixed() { return 31; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        List<int32> inner = heap List<int32>();\n"
        "        Wrapper<int32> w = heap Wrapper<>(inner);\n"
        "        return w.fixed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// `Outer<Inner<int32>>` — a templated type used as a type argument to another
// template. CajetaType::fromContext recursively resolves each typeArgument,
// so nested instantiation should fall out for free.

// Three-deep nesting: Outer<Mid<Inner<int32>>>. Each level instantiates the
// inner before the outer can be built. Verifies the recursion has no fixed
// depth limit.

// Constraint on a parameter whose argument is itself a template instantiation.
// `Pair extends Foo` ensures `Pair<int32, int32>` (an instantiation) still
// satisfies a `<T extends Foo>` bound — the isParentOrKind walk goes up
// through the instantiation's templateOrigin->superClasses chain.

// Multi-parameter diamond inference: ctor has both A- and B-typed slots,
// both must bind from the call args. Both args here are int32 — verifies
// the unifier correctly tracks per-parameter bindings even when the args
// happen to share a type.
