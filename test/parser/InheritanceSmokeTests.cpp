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

// P6.4 vtable-override fix: subclass overrides correctly dispatch
// through the parent's vtable slot. Pre-existing gap was that override
// detection keyed by full canonical (parent::name(params)) so the
// child's method went into a different slot than the parent's; calls
// through a base reference landed on the parent's body.

TEST(InheritanceSmokeTests, overrideDispatchesThroughBaseReference) {
    // Receiver typed as the base; call through the base's method name.
    // Should dispatch to Dog's override, not Animal's body.
    auto src =
        "package test;\n"
        "public class Animal {\n"
        "    public int32 speak() { return 1; }\n"
        "}\n"
        "public class Dog extends Animal {\n"
        "    public int32 speak() { return 42; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() {\n"
        "        Animal a = heap Dog();\n"
        "        return a.speak();\n"   // 42 — virtual dispatch to Dog::speak
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(InheritanceSmokeTests, overrideDispatchesFromParentMethodCallingOverridden) {
    // Parent's concrete method calls this.virtualMethod() — vtable
    // should land on the child's override even though the call site
    // is in the parent's source. This was the shape Stream.count()
    // hit (calling this.next() from Stream's body must dispatch to
    // ArrayStream::next()).
    auto src =
        "package test;\n"
        "public class Base {\n"
        "    public int32 produce() { return 10; }\n"
        "    public int32 consume() { return this.produce() + 1; }\n"
        "}\n"
        "public class Sub extends Base {\n"
        "    public int32 produce() { return 100; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() {\n"
        "        Sub s = heap Sub();\n"
        "        return s.consume();\n"  // produce() dispatches to Sub::produce → 101
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 101);
}

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


// Child in one file extends parent in another. Parses alphabetically:
// "test.AChild" before "test.ZParent". Until cross-module vtable/RTTI
// references were promoted to extern decls (see
// CajetaModule::ensureGlobalInModule / ensureFunctionInModule), the
// merged IR had `<badref>` placeholders in AChild's vtable initializer
// (parent-vtable slot + inherited-method slot) and verifyModule SEGV'd.
// Child in one file extends parent in another. Parses alphabetically:
// "test.AChild" before "test.ZParent". Until cross-module vtable/RTTI
// references were promoted to extern decls (see
// CajetaModule::ensureGlobalInModule / ensureFunctionInModule), the
// merged IR had `<badref>` placeholders in AChild's vtable initializer
// (parent-vtable slot + inherited-method slot) and verifyModule SEGV'd.
// Child in one file extends parent in another. Parses alphabetically
// so AChild parses before ZParent — exercises the deferred-prototype
// machinery (CajetaModule::buildPendingPrototypes) and the cross-
// module vtable/RTTI extern-decl fixup
// (CajetaModule::ensureGlobalInModule / ensureFunctionInModule).
//
// Behavior verified:
//   1. AChild's struct picks up ZParent's inherited field at the
//      right slot (read returns the value written through this).
//   2. Cross-file `heap AChild()` resolves the vtable pointer through
//      the merged module's resolved-extern decls (would crash on
//      `<badref>` before the fixup).
//   3. Reading c.inherited produces i32, not the field-GEP pointer
//      (ReturnStatement walks the inheritance chain to find the
//      property's type).
//
// Super-constructor chaining is a separate gap (AChild() does not
// implicitly call ZParent()), so this test sets the inherited slot
// through `this` in the child's ctor.
// Implicit super-constructor chaining. Child has a no-arg ctor with
// an empty body; parent has a no-arg ctor that initializes a field.
// Without implicit super, the field stays zero; with it, the parent
// ctor runs before the child's body and the field reads back as 41.
TEST(InheritanceSmokeTests, implicitSuperCallChainsParentCtor) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public int32 inherited;\n"
        "    public P() { this.inherited = 41; }\n"
        "}\n"
        "public class C extends P {\n"
        "    public C() { return; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() {\n"
        "        C c = heap C();\n"
        "        return c.inherited;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 41);
}

// Three-level chain — implicit super propagates all the way to the
// root. Each level's ctor stamps a different field; the test reads
// all three through the deepest instance.
TEST(InheritanceSmokeTests, implicitSuperChainsThreeLevels) {
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 a;\n"
        "    public A() { this.a = 1; }\n"
        "}\n"
        "public class B extends A {\n"
        "    public int32 b;\n"
        "    public B() { this.b = 2; }\n"
        "}\n"
        "public class C extends B {\n"
        "    public int32 c;\n"
        "    public C() { this.c = 4; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int32 run() {\n"
        "        C c = heap C();\n"
        "        return c.a + c.b + c.c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Cross-file inheritance end-to-end: deferred prototypes
// (CajetaModule::buildPendingPrototypes), cross-module extern-decl
// fixup (ensureGlobalInModule / ensureFunctionInModule), and
// implicit super-constructor chaining all have to work for the
// `heap AChild()` here to lay out the inherited field, run ZParent's
// ctor, and load the resulting value through c.inherited.
TEST(InheritanceSmokeTests, crossFileChildExtendsParentInOtherFile) {
    std::map<std::string, std::string> sources;
    sources["test.ZParent"] =
        "package test;\n"
        "public class ZParent {\n"
        "    public int32 inherited;\n"
        "    public ZParent() { this.inherited = 41; }\n"
        "    public int32 parentMethod() { return 1; }\n"
        "}\n";
    sources["test.AChild"] =
        "package test;\n"
        "public class AChild extends ZParent {\n"
        "    public AChild() { return; }\n"
        "    public int32 childMethod() { return 2; }\n"
        "    public static int32 run() {\n"
        "        AChild c = heap AChild();\n"
        "        return c.inherited;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.AChild");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 41);
}

// --- Auto-extend Object ----------------------------------------------------
//
// Every class without an explicit `extends` clause implicitly inherits
// cajeta.lang.Object. This test instantiates a bare class and calls
// `hash()` on it — a method NOT declared on the bare class itself.
// If auto-extend wired up correctly, name lookup walks the parent
// chain to Object.hash(), which bridges through @Native to the
// __cajeta_object_hash runtime function (identity-mixed-with-seed).
//
// Verification is structural: two calls on the same instance must
// match (deterministic), and the hash of any object must be non-zero
// (the seed initializer never produces zero — see
// __cajeta_hash_seed_init).

TEST(InheritanceSmokeTests, bareClassInheritsObjectHash) {
    auto src =
        "package test;\n"
        "public class Bare {\n"
        "    public Bare() { return; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int64 hashOnce() {\n"
        "        Bare b = heap Bare();\n"
        "        return b.hash();\n"
        "    }\n"
        "    public static int64 hashTwiceSameInstance() {\n"
        "        Bare b = heap Bare();\n"
        "        int64 h1 = b.hash();\n"
        "        int64 h2 = b.hash();\n"
        "        return h1 == h2 ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.I");
    auto hashOnce = jit->lookup<int64_t (*)()>("hashOnce");
    auto twiceSame = jit->lookup<int64_t (*)()>("hashTwiceSameInstance");
    EXPECT_NE(hashOnce(), 0);
    EXPECT_EQ(twiceSame(), 1);
}

// --- Manual hash() override -----------------------------------------------
//
// Object.hash() returns identity by default (cut 1). Classes that want
// value-keyed semantics override hash() manually — same shape as
// Java's `hashCode()`. A future `@AutoHash` annotation will synthesize
// a structural override for opted-in classes; until then, manual is
// the path.

TEST(InheritanceSmokeTests, manualHashOverrideWins) {
    // The user's manual hash() takes precedence over the inherited
    // identity hash. Probe: manual hash() always returns 12345.
    auto src =
        "package test;\n"
        "public class Custom {\n"
        "    public int32 x;\n"
        "    public Custom() { return; }\n"
        "    public int64 hash() { return 12345; }\n"
        "}\n"
        "public final class I {\n"
        "    public static int64 hashCustom() {\n"
        "        Custom c = heap Custom();\n"
        "        c.x = 7;\n"
        "        return c.hash();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.I");
    auto fn = jit->lookup<int64_t (*)()>("hashCustom");
    EXPECT_EQ(fn(), 12345);
}
