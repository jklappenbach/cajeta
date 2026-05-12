//
// Behavioral tests for dynamic dispatch through the vtable.
//
// Each class's instance reserves slot 0 for a vtable pointer; `new
// ClassName()` writes the class's `#VTable` global into that slot. The
// vtable is a sorted array of (FNV-1a hash, function pointer) pairs; calls
// hash the method's canonical signature, binary-search the vtable, and
// indirect-call.
//
// The hash-based scheme means multi-inheritance just works — methods have
// stable identity across classes regardless of how many bases combine.
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

// --- Single-class instance + method call ----------------------------------

TEST(DynamicDispatchTests, singleClassDispatchesToItsOwnMethod) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 value() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = new Counter();\n"
        "        return c.value();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- The crown jewel: override dispatch -----------------------------------

TEST(DynamicDispatchTests, overrideDispatchesToSubclassMethod) {
    // `dog.speak()` on a Dog instance — even though the static type Dog
    // *could* statically resolve to Dog::speak, the dispatch goes through
    // the vtable. Verify the vtable lookup returns Dog::speak, not
    // Animal::speak (which would happen if dispatch was static-only).
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 speak() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 speak() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Dog d = new Dog();\n"
        "        return d.speak();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(DynamicDispatchTests, inheritedMethodReachesParentImpl) {
    // Parent has a method the child doesn't override; calling it on a
    // child instance should still work — the child's vtable carries the
    // inherited entry (parent's function pointer).
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 legs() { return 4; }\n"
        "}\n"
        "public class Cat extends Animal {\n"
        "    public int32 lives() { return 9; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cat c = new Cat();\n"
        "        return c.legs();\n"     // inherited from Animal
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

TEST(DynamicDispatchTests, threeLevelChainDispatchesToDeepestOverride) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 name() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "    public int32 name() { return 2; }\n"
        "}\n"
        "public class C extends B {\n"
        "    public int32 name() { return 3; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        C c = new C();\n"
        "        return c.name();\n"     // C's override wins
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(DynamicDispatchTests, twoSubclassesDispatchIndependently) {
    // Square and Triangle both extend Shape and both override `area`.
    // Each instance's vtable carries its own override; the sum of their
    // areas verifies dispatch picks the right function for each.
    auto src =
        "package test;\n"
        "public class Shape {\n"
        "    public int32 area() { return 0; }\n"
        "}\n"
        "public class Square extends Shape {\n"
        "    public int32 area() { return 16; }\n"
        "}\n"
        "public class Triangle extends Shape {\n"
        "    public int32 area() { return 9; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Square s = new Square();\n"
        "        Triangle t = new Triangle();\n"
        "        return s.area() + t.area();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

// --- Multiple inheritance — the hash-based scheme makes this just work ----

TEST(DynamicDispatchTests, multipleInheritanceDispatchesCorrectly) {
    // C inherits from both A and B. Each parent has a uniquely-named
    // method. The hash-based vtable stores both at hash-determined
    // positions, and dispatch finds them regardless of inheritance order.
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 fromA() { return 10; }\n"
        "}\n"
        "public class B {\n"
        "    public int32 fromB() { return 20; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "    public int32 fromC() { return 30; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        C c = new C();\n"
        "        return c.fromA() + c.fromB() + c.fromC();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 60);
}

TEST(DynamicDispatchTests, multipleInheritanceWithOverride) {
    // C overrides a method that A introduced. Dispatch finds C's version
    // on a C instance, regardless of B's presence in the inheritance list.
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 shared() { return 1; }\n"
        "}\n"
        "public class B {\n"
        "    public int32 onlyB() { return 100; }\n"
        "}\n"
        "public class C extends A, B {\n"
        "    public int32 shared() { return 7; }\n"     // overrides A::shared
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        C c = new C();\n"
        "        return c.shared() + c.onlyB();\n"      // 7 + 100
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 107);
}
