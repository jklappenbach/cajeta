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
// destructor that heap-allocates a Probe instance contributes +1 to
// the drop count (the Probe's own drop entry firing at the
// destructor's scope exit). So a virtually-dispatched ~Dog() that
// allocates a Probe contributes 2 total (drop entry's pre-increment
// + the Probe drop).
//
// NOTE (task #15): the probe is a heap CLASS instance, not a primitive
// heap array. Since frame-arena U3 (37248bbf) arena-routes non-escaping
// single-dim primitive heap arrays — bump-allocated + reclaimed by scope
// reset, no free_array drop — a `heap int32[]` inside a destructor no
// longer ticks dropCount. Class instances are still drop-tracked (the
// arena opt explicitly defers them), so they remain a stable probe.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

// The crown jewel: declared type Animal, dynamic type Dog. Without
// virtual drop dispatch this returns 1 (Animal has no user drop, so just
// the entry's pre-increment). With it, this returns 2 (Dog's destructor
// fires via the vtable and its Probe allocation contributes the second
// drop).
TEST(VirtualDropDispatchTests, basetypedLocalCallsDerivedDestructor) {
    auto src =
        "package test;\n"
        "public class Probe {}\n"
        "public class Animal {\n"
        "    public int32 sound() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public ~Dog() {\n"
        "        Probe junk = heap Probe();\n"
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
        "public class Probe {}\n"
        "public class Tagged {\n"
        "    public ~Tagged() {\n"
        "        Probe junk = heap Probe();\n"
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
// base-typed local, virtual dispatch enters ~Derived first
// (proving the static base type doesn't shadow the dynamic type's
// drop_fn), and from task #151 the chain then implicitly runs
// ~Base afterward. So the count picks up Probe drops from both
// destructors. Pinning two properties at once: virtual dispatch
// AND the chain.
TEST(VirtualDropDispatchTests, derivedDestructorWinsOverBase) {
    auto src =
        "package test;\n"
        "public class Probe {}\n"
        "public class Base {\n"
        "    public ~Base() {\n"
        "        Probe one = heap Probe();\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public ~Derived() {\n"
        "        Probe a = heap Probe();\n"
        "        Probe b = heap Probe();\n"
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
    // 1 (entry) + 2 (two Probe drops in ~Derived)
    //         + 1 (one Probe drop in ~Base via the chain)
    //         = 4. Pre-chain (task #151) this was 3 — only ~Derived
    // ran. The +1 is the chain firing ~Base after ~Derived returns.
    EXPECT_EQ(jit->lookup<int64_t (*)()>("read")(), 4);
}
