//
// ReifiedCaptureTests — reified-capture-plan.md: recovering a concrete
// instantiation from a template wildcard. Because cajeta monomorphizes, a value
// widened to `Box<?>` is still its concrete `Box<int32>` at runtime, so
// `instanceof Box<int32>` is a sound reified RTTI check and `(Box<int32>) w`
// recovers the typed handle (representation-identical — same pointer).
//

#include <gtest/gtest.h>

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

const char* BOX =
    "package test;\n"
    "public class Box<T> {\n"
    "    T value;\n"
    "    public Box(T v) { this.value = v; }\n"
    "    public T get() { return this.value; }\n"
    "}\n";

} // namespace

// instanceof over a wildcard: true for the real instantiation, false for another.
TEST(ReifiedCaptureTests, instanceofMatchesReifiedInstantiation) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Box<int32>) { r = r + 1; }\n"
        "        if (w instanceof Box<float32>) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// pattern binding: `w instanceof Box<int32> f` binds `f : Box<int32>` in the
// matched branch; reading through it sees the concrete object.
TEST(ReifiedCaptureTests, instanceofBindingCapture) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(99);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<int32> f) {\n"
        "            return f.get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// pattern binding on a mismatched dtype does not enter the branch.
TEST(ReifiedCaptureTests, instanceofBindingMismatchSkips) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<float32> f) {\n"
        "            return 1;\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), -1);
}

// tryAs on a MATCH: returns a present Optional holding the same object.
TEST(ReifiedCaptureTests, tryAsPresentOnMatch) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        Optional<Box<int32>> o = w.tryAs<Box<int32>>();\n"
        "        if (o.isPresent()) { return o.get().get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// tryAs on a MISMATCH: returns an empty Optional (no throw, no mis-typed ptr).
TEST(ReifiedCaptureTests, tryAsEmptyOnMismatch) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        Optional<Box<float32>> o = w.tryAs<Box<float32>>();\n"
        "        if (o.isEmpty()) { return 99; }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// capture: a guarded cast recovers the concrete handle and reads through it.
TEST(ReifiedCaptureTests, capturedCastReadsConcrete) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        if (w instanceof Box<int32>) {\n"
        "            Box<int32> cap = (Box<int32>) w;\n"
        "            return cap.get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// unguarded capture cast on a MATCH: no instanceof guard, the reified check
// passes, the concrete handle is recovered and read.
TEST(ReifiedCaptureTests, unguardedCastSucceedsOnMatch) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(42);\n"
        "        Box<?> w = bi;\n"
        "        Box<int32> cap = (Box<int32>) w;\n"
        "        return cap.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- item 1: nested + supertype capture targets (capture plan 5a) ---------

// NESTED target — `Box<Box<int32>>` widened to `Box<?>`. The reified match is
// recursive: instanceof the exact nested instantiation is true, instanceof a
// nested instantiation that differs only in the INNER arg is false.
TEST(ReifiedCaptureTests, instanceofMatchesNestedInstantiation) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Box<int32>> bbi = heap Box<Box<int32>>(heap Box<int32>(7));\n"
        "        Box<?> w = bbi;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Box<Box<int32>>) { r = r + 1; }\n"
        "        if (w instanceof Box<Box<float32>>) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// NESTED capture cast — recover `Box<Box<int32>>` from `Box<?>` and read both
// levels through the captured handle (same pointer, no copy).
TEST(ReifiedCaptureTests, capturedCastReadsNested) {
    std::string src = std::string(BOX) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Box<int32>> bbi = heap Box<Box<int32>>(heap Box<int32>(42));\n"
        "        Box<?> w = bbi;\n"
        "        Box<Box<int32>> cap = (Box<Box<int32>>) w;\n"
        "        return cap.get().get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// SUPERTYPE target — `List<T> extends Container<T>`. A `List<int32>` value held
// as `Container<?>` matches BOTH its exact instantiation (`List<int32>`) and its
// template SUPERtype instantiation (`Container<int32>`), but not a wrong arg
// (`Container<float32>`). The is-a check walks the reified parent chain.
TEST(ReifiedCaptureTests, instanceofMatchesTemplateSupertype) {
    std::string src =
        "package test;\n"
        "public class Container<U> {\n"
        "    public int32 count() { return 41; }\n"
        "}\n"
        "public class List<T> extends Container<T> {\n"
        "    public int32 listMark() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        List<int32> l = heap List<int32>();\n"
        "        Container<?> w = l;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Container<int32>) { r = r + 1; }\n"
        "        if (w instanceof List<int32>) { r = r + 100; }\n"
        "        if (w instanceof Container<float32>) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 101);
}

// SUPERTYPE capture cast — recover the concrete supertype instantiation
// `Container<int32>` from `Container<?>` and dispatch the inherited method.
TEST(ReifiedCaptureTests, capturedCastToTemplateSupertype) {
    std::string src =
        "package test;\n"
        "public class Container<U> {\n"
        "    public int32 count() { return 41; }\n"
        "}\n"
        "public class List<T> extends Container<T> {\n"
        "    public int32 listMark() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        List<int32> l = heap List<int32>();\n"
        "        Container<?> w = l;\n"
        "        Container<int32> cap = (Container<int32>) w;\n"
        "        return cap.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 41);
}

// SUPERTYPE capture down to the EXACT subtype — `Container<?>` that is really a
// `List<int32>` recovers to `List<int32>` and reaches the subtype-only method.
TEST(ReifiedCaptureTests, capturedCastToExactSubtypeThroughSupertypeWildcard) {
    std::string src =
        "package test;\n"
        "public class Container<U> {\n"
        "    public int32 count() { return 41; }\n"
        "}\n"
        "public class List<T> extends Container<T> {\n"
        "    public int32 listMark() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        List<int32> l = heap List<int32>();\n"
        "        Container<?> w = l;\n"
        "        List<int32> cap = (List<int32>) w;\n"
        "        return cap.listMark();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// unguarded capture cast on a MISMATCH: throws cajeta.error.ClassCastException
// (catchable) rather than handing back a mis-typed pointer (UB).
TEST(ReifiedCaptureTests, unguardedCastThrowsClassCastException) {
    std::string src =
        "package test;\n"
        "import cajeta.error.ClassCastException;\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> bi = heap Box<int32>(7);\n"
        "        Box<?> w = bi;\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            Box<float32> bad = (Box<float32>) w;\n"
        "            result = 100;\n"
        "        } catch (ClassCastException e) {\n"
        "            result = 7;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- item 2: name -> RTTI registry (class-bounded-wildcard precursor) ------

// `__cajeta_rtti_for_name` maps a type's canonical name -> its CajetaRtti* over
// the REFL-8 registry (#ClassObject = { vtable, rtti }, rtti at offset 8), so the
// capture-site lowering for `(Foo<? extends Animal>) w` can recover the element
// type's hierarchy from the NAME the container RTTI stores. This is the lookup
// CONTRACT, unit-tested against the PROCESS registry — host C++ and
// __cajeta_register_class share the same statically-linked runtime copy, whereas
// the JIT embeds its OWN runtime copy ([[nvidia-on-device-validation]]: host C++
// can't see JIT-local statics). The real-data path (JIT ctors populate the
// embedded registry; the capture site reads it) is covered end-to-end by the
// class-bounded-wildcard capture tests (item 3), which run inside the JIT.
extern "C" {
    void* __cajeta_rtti_for_name(const char* name);
    void __cajeta_register_class(const char* name, void* classObject);
}

TEST(ReifiedCaptureTests, rttiForNameResolvesRegisteredCanonicalName) {
    // #ClassObject layout the registry stores: { ptr Class#VTable, ptr rtti }.
    // Static storage so the registry's borrowed pointer stays valid for the
    // process lifetime (the table is never freed). A sentinel rtti value lets us
    // prove the helper returns the offset-8 slot, not merely non-null.
    static void* sentinelRtti = reinterpret_cast<void*>(0xCAFEF00D);
    static void* fakeClassObject[2] = { /*vtable*/ nullptr, /*rtti*/ sentinelRtti };
    __cajeta_register_class("test.FakeWidgetForNameLookup", fakeClassObject);

    // hit: returns exactly the rtti pointer at offset 8 of the registered object.
    EXPECT_EQ(__cajeta_rtti_for_name("test.FakeWidgetForNameLookup"), sentinelRtti);
    // miss: an unregistered canonical name resolves to NULL — the capture site
    // treats this as "cannot prove the bound" and fails the capture safely.
    EXPECT_EQ(__cajeta_rtti_for_name("test.UnregisteredZzzName"), nullptr);
    // null arg is tolerated (no crash) and returns NULL.
    EXPECT_EQ(__cajeta_rtti_for_name(nullptr), nullptr);
}

// --- item 3: class-bounded-wildcard capture (capture plan 5b) --------------

// A class hierarchy where Dog IS-A Animal and Cat is NOT, plus a Box<T>
// container, so `Box<? extends Animal>` admits Box<Dog> and rejects Box<Cat>.
const char* ANIMALS =
    "package test;\n"
    "import cajeta.error.ClassCastException;\n"
    "public class Animal { public int32 kind() { return 1; } }\n"
    "public class Dog extends Animal { public int32 bark() { return 2; } }\n"
    "public class Cat { public int32 meow() { return 3; } }\n"
    "public class Box<T> {\n"
    "    T value;\n"
    "    public Box(T v) { this.value = v; }\n"
    "    public T get() { return this.value; }\n"
    "}\n";

// instanceof a class-bounded wildcard: true when the reified element type
// conforms to the bound (Dog <: Animal).
TEST(ReifiedCaptureTests, instanceofBoundedWildcardMatchesSubtype) {
    std::string src = std::string(ANIMALS) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Dog> bd = heap Box<Dog>(heap Dog());\n"
        "        Box<?> w = bd;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Box<? extends Animal>) { r = r + 1; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// instanceof a class-bounded wildcard: false when the element type does NOT
// conform to the bound (Cat is not an Animal).
TEST(ReifiedCaptureTests, instanceofBoundedWildcardRejectsNonSubtype) {
    std::string src = std::string(ANIMALS) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Cat> bc = heap Box<Cat>(heap Cat());\n"
        "        Box<?> w = bc;\n"
        "        int32 r = 0;\n"
        "        if (w instanceof Box<? extends Animal>) { r = r + 1; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// checked cast to a class-bounded wildcard SUCCEEDS when the element conforms:
// no throw, the (widened) handle is recovered.
TEST(ReifiedCaptureTests, capturedCastBoundedWildcardSucceedsOnSubtype) {
    std::string src = std::string(ANIMALS) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Dog> bd = heap Box<Dog>(heap Dog());\n"
        "        Box<?> w = bd;\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            Box<?> cap = (Box<? extends Animal>) w;\n"
        "            result = 1;\n"
        "        } catch (ClassCastException e) {\n"
        "            result = 0;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// checked cast to a class-bounded wildcard THROWS when the element does not
// conform (Cat is not an Animal) — never hands back a mis-bounded handle.
TEST(ReifiedCaptureTests, capturedCastBoundedWildcardThrowsOnNonSubtype) {
    std::string src = std::string(ANIMALS) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<Cat> bc = heap Box<Cat>(heap Cat());\n"
        "        Box<?> w = bc;\n"
        "        int32 result = -1;\n"
        "        try {\n"
        "            Box<?> cap = (Box<? extends Animal>) w;\n"
        "            result = 1;\n"
        "        } catch (ClassCastException e) {\n"
        "            result = 0;\n"
        "        }\n"
        "        return result;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
