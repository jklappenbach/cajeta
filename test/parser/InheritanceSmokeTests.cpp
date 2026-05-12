//
// Inheritance smoke tests.
//
// `CajetaClass::resolveSuperClasses` walks the qExtended list and looks
// parents up in `module->getStructures()`. Parents must be declared earlier
// in the source than their subclasses (no forward refs in v1).
//
// What's exercised here:
//   - A class extending another compiles end-to-end (prototype gen,
//     vtable build, RTTI build for both).
//   - Multi-level chains (A → B → C) compile.
//   - Multiple subclasses sharing a parent compile.
//   - The vtable's slot-reuse semantic for overrides: the override and the
//     parent's method land at the same `virtualTableIndex`. Verified
//     indirectly here via successful compile + module verify; a behavioral
//     test (`dog.speak()` actually dispatches to Dog's override) lands when
//     MethodCallExpression learns to indirect through the vtable.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.I");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Single-level inheritance ----------------------------------------------

TEST(InheritanceSmokeTests, simpleExtendsCompiles) {
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 speak() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 fetch() { return 2; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() { return 11; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// --- Override reuses the parent's slot -------------------------------------

TEST(InheritanceSmokeTests, overrideReusesParentSlot) {
    // Dog declares `speak()` with the same canonical signature as Animal's.
    // buildVirtualTable walks parent-first, assigns Animal::speak slot 0;
    // then sees Dog::speak with the same canonical → reuses slot 0.
    // The vtable's slot 0 is rewritten to point to Dog::speak.
    //
    // We can't easily inspect the slot count from the C++ side; what we CAN
    // verify is that the module compiles without LLVM rejecting the vtable
    // (a duplicate slot would produce a wrong-arity constant). Both classes'
    // RTTI globals appear in the module too.
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 speak() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 speak() { return 42; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() { return 99; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// --- Multi-level inheritance chain -----------------------------------------

TEST(InheritanceSmokeTests, threeLevelChainCompiles) {
    // A's methods get slots first; B may add its own; C may override or
    // append. The recursion in buildVirtualTable walks all three.
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 fromA() { return 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "    public int32 fromB() { return 2; }\n"
        "}\n"
        "public class C extends B {\n"
        "    public int32 fromC() { return 3; }\n"
        "    public int32 fromA() { return 100; }\n"   // override at slot 0
        "}\n"
        "public final class I {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Multiple subclasses sharing a parent ----------------------------------

TEST(InheritanceSmokeTests, multipleSubclassesShareParent) {
    auto src =
        "package test;\n"
        "public class Shape {\n"
        "    public int32 area() { return 0; }\n"
        "}\n"
        "public class Square extends Shape {\n"
        "    public int32 area() { return 4; }\n"
        "}\n"
        "public class Triangle extends Shape {\n"
        "    public int32 area() { return 3; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() { return 17; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// --- Child adds methods on top of parent -----------------------------------

TEST(InheritanceSmokeTests, childExtendsWithoutOverriding) {
    // Parent has `inheritedMethod`; child adds `newMethod`. Child's vtable
    // should have 2 slots: one inherited (pointing to Parent::inheritedMethod
    // since Child doesn't override), one new (Child::newMethod).
    auto src =
        "package test;\n"
        "public class Parent {\n"
        "    public int32 inheritedMethod() { return 10; }\n"
        "}\n"
        "public class Child extends Parent {\n"
        "    public int32 newMethod() { return 20; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() { return 30; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30);
}
