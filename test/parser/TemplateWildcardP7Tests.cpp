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
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// A read through an unprojected wildcard cannot resolve, so the compile must
// FAIL rather than lower the call to null and return 0.
//
// KNOWN WART (filed, silent-resolution-diagnostics 2.2.4): the message names the
// receiver as the ENCLOSING class (`no member 'tag' on 'test.D'`) rather than the
// wildcard receiver. `b.get()` yields the unprojected sentinel, and the chained
// `.tag()` falls back to the enclosing class as its target. The rejection is
// right; the receiver name in the text is not. Asserted on the id, not the
// receiver, so this test pins the SEMANTICS and does not cement the wart.
void expectUnresolved(const std::string& src) {
    try {
        runI32(src);
        FAIL() << "expected the unresolvable wildcard read to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MEMBER_NOT_FOUND")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
    }
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

// Field-read projection. `b.value` on a `Box<? extends Animal>`
// receiver resolves to Animal so chained member lookup works the same
// as the method-return path above. Without projection on field reads,
// `b.value.tag()` falls through with the wildcard-sentinel-on-the-LHS
// problem and codegen emits the null-return diagnostic.
TEST(TemplateWildcardP7Tests, extendsBoundProjectsThroughFieldRead) {
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
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<Dog> bDog = heap Box<Dog>(d);\n"
        "        Box<? extends Animal> b = bDog;\n"
        "        return b.value.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Wildcard parameter receives a concrete instantiation. Inside the
// callee, the parameter is bounded-wildcard typed; field-read and
// method-return projections should compose so chained member lookup
// works without the caller needing to know the bound.
//
// This test exercises whether the call-site assignability check + the
// in-body projections compose. Concrete `Box<Dog>` should be a valid
// argument for a `Box<? extends Animal>` parameter (covariant subtype
// rule already shipped — see TemplateWildcardP6Tests), and inside the
// callee the field-read projection from P7 should give Animal.
TEST(TemplateWildcardP7Tests, extendsBoundParameterAcceptsConcreteAndProjects) {
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
        "}\n"
        "public final class D {\n"
        "    public static int32 inspect(Box<? extends Animal> b) {\n"
        "        return b.value.tag();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<Dog> bDog = heap Box<Dog>(d);\n"
        "        return inspect(bDog);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// `? super B` accepts B-and-narrower writes — the PECS consumer side.
// Given `Box<? super Dog> b` (declared upcast from a `Box<Animal>`),
// writing a fresh Dog to `b.value` is the safe write direction:
// every supertype of Dog (Animal, Object, ...) can hold a Dog. After
// the write, the underlying Box<Animal>'s value field carries the new
// Dog, and reading through the original Animal-typed reference works
// because Dog is-a Animal.
TEST(TemplateWildcardP7Tests, superBoundAcceptsNarrowerWrite) {
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
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Animal seed = heap Animal();\n"
        "        Box<Animal> bAnimal = heap Box<Animal>(seed);\n"
        "        Box<? super Dog> b = bAnimal;\n"
        "        Dog d = heap Dog();\n"
        "        b.value = d;\n"
        "        return bAnimal.value.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Compose super-write with the wildcard-parameter compatibility from
// the previous slice. `static void writeNarrower(Box<? super Dog> b, Dog d)`
// must accept a `Box<Animal>` arg (Animal is a supertype of Dog) and
// then write d into b.value inside the body. The write threads through
// to the caller's bAnimal because both alias the same heap instance.
TEST(TemplateWildcardP7Tests, superBoundParameterComposesWithWrite) {
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
        "}\n"
        "public final class D {\n"
        "    public static void writeNarrower(Box<? super Dog> b, Dog d) {\n"
        "        b.value = d;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Animal seed = heap Animal();\n"
        "        Box<Animal> bAnimal = heap Box<Animal>(seed);\n"
        "        Dog d = heap Dog();\n"
        "        writeNarrower(bAnimal, d);\n"
        "        return bAnimal.value.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Direct construction into a wildcard-typed local. Skips the
// named-intermediate (`Box<Dog> bDog = ...; Box<? extends Animal> b = bDog;`)
// pattern in case that path masks a missing covariance check on the
// initializer expression itself.
TEST(TemplateWildcardP7Tests, extendsBoundLocalAcceptsDirectHeapConstruction) {
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
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<? extends Animal> b = heap Box<Dog>(d);\n"
        "        return b.value.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// Wildcard-receiver method-CALL dispatch with a T-parametric formal.
// Substitution-stable hash means the wildcard's alias hash for
// `set(T)` is the same one Box<Dog>#VTable carries — so the runtime
// vtable lookup lands on Box<Dog>::set even though the static type
// is `Box<? extends Animal>`.
TEST(TemplateWildcardP7Tests, wildcardReceiverDispatchesTParamMethod) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 tag() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 tag() { return 2; }\n"
        "}\n"
        "public class Counter {\n"
        "    public static int32 setCalls = 0;\n"
        "}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "    public void set(T v) {\n"
        "        Counter.setCalls = Counter.setCalls + 1;\n"
        "        this.value = v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Box<? extends Animal> b = heap Box<Dog>(d);\n"
        "        b.set(b.get());\n"
        "        return Counter.setCalls * 100 + b.value.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 102);
}

// PECS write-soundness. `Box<? extends Animal>::set(? extends Animal)`
// must reject a foreign Animal-typed argument — the receiver's actual
// T could be a strict subtype like Dog, and writing an Animal would
// violate the container's element-type contract. Capture identity
// would allow the write if the value came from the same receiver
// (covered above), but a freshly-allocated Animal carries no such
// origin.
//
// Today the call silently fails to resolve (subtypeDistance returns -1,
// resolveMethod returns null, invokeMethod produces a no-op). For
// soundness, the compiler should reject with a clear diagnostic so
// the user sees the violation rather than wondering why the write
// disappeared.
TEST(TemplateWildcardP7Tests, pecsForeignWriteToExtendsRejected) {
    auto src =
        "package test;\n"
        "public class Animal { public int32 tag() { return 1; } }\n"
        "public class Dog extends Animal { public int32 tag() { return 2; } }\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public void set(T v) { this.value = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = heap Dog();\n"
        "        Animal foreign = heap Animal();\n"
        "        Box<? extends Animal> b = heap Box<Dog>(d);\n"
        "        b.set(foreign);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected PECS write rejection on Box<? extends Animal>::set(foreign)";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_PECS_WRITE_VIOLATION")
            << "expected CAJETA_ERROR_PECS_WRITE_VIOLATION, got: "
            << e.getErrorId() << " — " << e.getMessage();
    }
}

// V1 scope guard: unbounded wildcard `Box<?>::get()` does NOT project
// (no bound to project to), so the chained `.tag()` cannot resolve against
// the raw wildcard sentinel.
//
// This USED to pin `runI32(src) == 0` — i.e. it asserted that the
// unresolvable `.tag()` silently lowered to null and the method returned 0.
// That was pinning a miscompile as the contract. Since
// silent-resolution-diagnostics Unit 1 the null-in-value-position backstop
// rejects it, which is what "does not project" should always have meant:
// a compile error, not a zero. The scope guard is unchanged — only its
// observable form is (silent 0 -> located diagnostic).
// PARKED (2026-07-13): member-not-found is held back on main so tools/mcp
// compiles (it reads String's `byteLength`/`bytes` as FIELDS; the 36779177
// re-core made them methods). Re-enable with that repair. Work lives on
// feature/silent-resolution-diagnostics @ f086c73e; see the plan's 1.3.3.
TEST(TemplateWildcardP7Tests, DISABLED_unboundedDoesNotProject) {
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
    expectUnresolved(src);
}

// V1 scope guard: `? super B` is contravariant (write-only direction
// from the consumer's perspective; reads project to the top type, not
// to B). We leave it unprojected in v1 — a `Box<? super Animal>` read
// position keeps the wildcard sentinel and `.tag()` fails to resolve.
// As with unboundedDoesNotProject, "fails to resolve" is now a compile
// error rather than a silent 0.
// PARKED (2026-07-13): member-not-found is held back on main so tools/mcp
// compiles (it reads String's `byteLength`/`bytes` as FIELDS; the 36779177
// re-core made them methods). Re-enable with that repair. Work lives on
// feature/silent-resolution-diagnostics @ f086c73e; see the plan's 1.3.3.
TEST(TemplateWildcardP7Tests, DISABLED_superDoesNotProject) {
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
    expectUnresolved(src);
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
