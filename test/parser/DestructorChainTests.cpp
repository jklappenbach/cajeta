//
// Implicit destructor chaining (C++ semantics) — task #151.
//
// When `Derived extends Base` is destroyed, `~Derived()` runs first
// followed implicitly by `~Base()`. The user doesn't write a `super()`
// call; the compiler synthesizes the chain. Multi-level hierarchies
// chain through every level. Through a base-typed local holding a
// derived instance, virtual dispatch still picks the dynamic type's
// destructor, which then chains up.
//
// Observation pattern (mirrors ClassDropTests / VirtualDropDispatchTests):
// each destructor that allocates a heap array contributes +1 to the
// drop count (the array's own drop entry firing at the destructor's
// scope exit). The outer local's drop entry's pre-increment adds 1.
// So the expected count is: 1 (entry) + Σ (array_count_per_destructor).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile + run a snippet that resets the drop counter, instantiates
// the local in `body`, and reads the final drop count.
int64_t observeChainDrops(const std::string& classes,
                          const std::string& localExpr) {
    std::string src =
        std::string("package test;\n") + classes +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        " + localExpr + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    jit->lookup<int32_t (*)()>("run")();
    return jit->lookup<int64_t (*)()>("read")();
}

} // namespace

// One level: Base + Derived, both with destructors. Through a
// Derived-typed local. ~Derived runs (1 array), then ~Base runs
// (1 array). + 1 for the drop-chain entry = 3.
TEST(DestructorChainTests, parentDestructorRunsAfterChild) {
    auto cls =
        "public class Base {\n"
        "    public ~Base() {\n"
        "        int32[] baseMark = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public ~Derived() {\n"
        "        int32[] derivedMark = new int32[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "Derived d = heap Derived();"), 3);
}

// Through a base-typed local — virtual dispatch picks ~Derived,
// which then chains to ~Base. Same expected count: 1 entry + 1
// derived array + 1 base array = 3.
TEST(DestructorChainTests, chainViaBaseTypedLocal) {
    auto cls =
        "public class Base {\n"
        "    public ~Base() {\n"
        "        int32[] baseMark = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public ~Derived() {\n"
        "        int32[] derivedMark = new int32[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "Base b = heap Derived();"), 3);
}

// Three-level chain A <- B <- C. All three destructors fire when a
// C-typed local is dropped. + 1 entry + 1 + 2 + 3 array allocations
// in each respective destructor = 7.
TEST(DestructorChainTests, threeLevelChain) {
    auto cls =
        "public class A {\n"
        "    public ~A() {\n"
        "        int32[] a1 = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class B extends A {\n"
        "    public ~B() {\n"
        "        int32[] b1 = new int32[1];\n"
        "        int32[] b2 = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class C extends B {\n"
        "    public ~C() {\n"
        "        int32[] c1 = new int32[1];\n"
        "        int32[] c2 = new int32[1];\n"
        "        int32[] c3 = new int32[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "C c = heap C();"), 7);
}

// Child destructor missing — only ~Base runs. + 1 entry + 1 array
// = 2. Pins that the chain doesn't require every level to declare a
// destructor (skipping levels in the chain that didn't write one).
TEST(DestructorChainTests, parentChainWhenChildHasNoDestructor) {
    auto cls =
        "public class Base {\n"
        "    public ~Base() {\n"
        "        int32[] baseMark = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public int32 field;\n"
        "    public Derived() { this.field = 7; }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "Derived d = heap Derived();"), 2);
}

// Parent destructor missing — only ~Derived runs. + 1 entry + 1
// array = 2. Symmetry guard.
TEST(DestructorChainTests, childRunsWhenParentHasNoDestructor) {
    auto cls =
        "public class Base {\n"
        "    public int32 field;\n"
        "    public Base() { this.field = 0; }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public ~Derived() {\n"
        "        int32[] derivedMark = new int32[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "Derived d = heap Derived();"), 2);
}

// Stack allocation chain. `stack Derived(...)` also invokes the
// destructor chain at scope exit. Stack-drop wrapper differs from
// heap (no __cajeta_free at the end) but the user-body chain shape
// should match.
TEST(DestructorChainTests, stackAllocChainsDestructors) {
    auto cls =
        "public class Base {\n"
        "    public Base() { return; }\n"
        "    public ~Base() {\n"
        "        int32[] baseMark = new int32[1];\n"
        "    }\n"
        "}\n"
        "public class Derived extends Base {\n"
        "    public Derived() { return; }\n"
        "    public ~Derived() {\n"
        "        int32[] derivedMark = new int32[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(observeChainDrops(cls,
        "Derived d = stack Derived();"), 3);
}
