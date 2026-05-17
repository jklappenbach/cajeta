//
// Gap 1 (MemoryModel.md § Known gaps): virtual dispatch on drop.
//
// When a class-typed local's declared type is a base class but its
// dynamic type is a subclass, scope-exit drop should invoke the
// SUBCLASS's destructor, not the base's. Today the drop fn is bound
// statically at drop-push time from the declared type — so
// `Animal a = heap Dog()` ends up calling `~Animal()` (a no-op when
// undefined), skipping `~Dog()` entirely.
//
// Fix: every class's vtable carries a drop_fn slot pointing at the
// class's own synthesized drop wrapper. The drop chain registers a
// runtime helper `__cajeta_class_virtual_drop` that loads the vtable
// from the instance and dispatches through that slot — so the dynamic
// type's destructor fires regardless of the declared type.
//
// Observability follows the existing pattern in ClassDropTests: a
// destructor that allocates a heap array contributes +1 to the drop
// count (the array's own drop entry firing at the destructor's scope
// exit). So a virtually-dispatched ~Dog() that allocates an array
// contributes 2 total (drop entry's pre-increment + the array drop).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

// The crown jewel: declared type Animal, dynamic type Dog. Today this
// returns 1 (Animal has no user drop, so just the entry's pre-increment).
// After the fix this returns 2 (Dog's destructor fires via the vtable
// and its array allocation contributes the second drop).
TEST(VirtualDropDispatchTests, basetypedLocalCallsDerivedDestructor) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 sound() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public ~Dog() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Animal a = heap Dog();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Sanity: when the declared type IS the concrete type, the same
// destructor still fires (regression guard — the virtual path must
// also work for non-polymorphic cases).
TEST(VirtualDropDispatchTests, concreteTypedLocalStillCallsDestructor) {
    auto src =
        "package test;\n"
        "public class Tagged {\n"
        "    public ~Tagged() {\n"
        "        int32[] junk = new int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Tagged t = heap Tagged();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 2);
}

// Base also has a user destructor; subclass overrides. Through a
// base-typed local, the override (subclass's) is what runs. Same
// idea as the first test but the base is no longer a no-op — proves
// dispatch goes to subclass even when base would have been a valid
// non-null function pointer.
TEST(VirtualDropDispatchTests, derivedDestructorWinsOverBase) {
    auto src =
        "package test;\n"
        "public class Base {\n"
        "    public ~Base() {\n"
        "        int32[] one = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public ~Derived() {\n"
        "        int32[] a = new int32[1];\n"
        "        int32[] b = new int32[1];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Base b = heap Derived();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    // Derived runs (not Base):  1 (entry) + 2 (two array drops in
    // ~Derived) = 3. If Base ran instead the count would be 2.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 3);
}
