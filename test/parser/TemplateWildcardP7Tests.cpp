// Step 7 of template wildcards — capture conversion.
//
// Goal: when a method is invoked on a bounded-wildcard receiver, the
// bound flows through the method's type-argument substitution at read
// positions. Concretely:
//
//   Box<? extends Animal> b = ...;
//   Animal a = b.get();   // get() returns T → bound; assignment must typecheck
//
// Today (pre-Step-7) `b.get()` returns the bare wildcard sentinel, so
// the assignment to `Animal` is either rejected or routes through a
// raw-pointer load that bypasses the contract. Step 7 substitutes the
// bound for T on extends-positions in the resolved return type so the
// caller sees `Animal` instead of `?`.
//
// Scope of this first slice (intentionally small, TDD red→green):
//   - `? extends B` only (covariant read direction).
//   - Single-arg generics (`Box<T>`).
//   - Return type only (no parameter / field-read site work yet).
//   - JIT compile + run; the return value proves the bound projected
//     through to runtime dispatch.
//
// Future slices (deferred):
//   - `? super B` contravariant write direction.
//   - Capture identity ("same unknown T" across two calls on the same
//     receiver; today the substitution treats each call independently).
//   - Field reads through wildcard receivers.
//   - Method parameters typed on the wildcard.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/type/CajetaType.h"

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

// `Box<? extends Animal>::get()` returns the bound Animal. The call's
// resolved return type carries through the substitution so `tag()`
// (declared on Animal) resolves directly on the call result — without
// the carry-through, the return is the raw wildcard sentinel and the
// `.tag()` lookup fails to find a method.
TEST(TemplateWildcardP7Tests, extendsBoundResolvesMemberOnReturn) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<Dog> bDog = heap Box<Dog>(d);\n"
        "        Box<? extends Animal> b = bDog;\n"
        "        return b.get().tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// V1 scope guard: unbounded wildcard `Box<?>::get()` does NOT project
// (no bound to project to). The chained `.tag()` lookup against the
// raw wildcard sentinel returns nothing meaningful and codegen falls
// through to the null-return path. The test pins that the JIT compiles
// and the runtime returns 0 (rather than 2 from Dog::tag, which would
// indicate the projection misfired and ran without a bound).
TEST(TemplateWildcardP7Tests, unboundedDoesNotProject) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<Dog> bDog = heap Box<Dog>(d);\n"
        "        Box<?> b = bDog;\n"
        "        return b.get().tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// V1 scope guard: `? super B` is contravariant (write-only direction
// from the consumer's perspective; reads project to the top type, not
// to B). We leave it unprojected in v1 — a `Box<? super Animal>` read
// position keeps the wildcard sentinel and `.tag()` fails to resolve.
TEST(TemplateWildcardP7Tests, superDoesNotProject) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<Dog> bDog = heap Box<Dog>(d);\n"
        "        Box<? super Animal> b = bDog;\n"
        "        return b.get().tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Unit test on the static helper. Confirms the projection rule
// directly so a future refactor can lean on the contract without
// re-running the JIT roundtrip.
TEST(TemplateWildcardP7Tests, captureProjectExtendsReturnsBound) {
    auto* sentinel = cajeta::CajetaType::wildcardSentinel().get();
    (void) sentinel;  // ensure the wildcard-init path ran for this test bin

    // For ? extends Bound, captureProject(?) == Bound.
    auto stringTy = cajeta::CajetaType::of("String");
    if (stringTy) {
        auto extWild = cajeta::CajetaType::wildcardSentinelExtends(stringTy);
        ASSERT_NE(extWild, nullptr);
        auto projected = cajeta::CajetaType::captureProject(extWild);
        EXPECT_EQ(projected.get(), stringTy.get())
            << "captureProject(? extends String) should be String";
    }

    // Unbounded wildcard projects to itself (no bound to use).
    auto unbounded = cajeta::CajetaType::wildcardSentinel();
    auto projectedUnbounded = cajeta::CajetaType::captureProject(unbounded);
    EXPECT_EQ(projectedUnbounded.get(), unbounded.get())
        << "captureProject(?) without a bound returns itself";

    // Null input maps to null.
    cajeta::CajetaTypePtr nullArg;
    auto projectedNull = cajeta::CajetaType::captureProject(nullArg);
    EXPECT_EQ(projectedNull, nullArg);
}
