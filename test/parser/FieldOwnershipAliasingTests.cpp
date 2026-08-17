//
// FieldOwnership.md § Solution B — verify the "drop all fields, no-op
// if owned elsewhere" rule via the live-allocation set.
//
// These tests pin the aliased-field shapes that the old "fields are
// owners" rule would have made impossible. The AutoFieldDropTests suite
// covers sole-owner fields in isolation; this suite covers the aliasing
// cases (one heap allocation referenced by both a local and a field, or
// by two fields). Pre-Solution-B these double-freed; post-Solution-B
// the live-set claim makes the second drop a silent no-op.
//
// What we observe: if the test reaches `return` without a glibc abort
// ("double free or corruption detected"), the runtime correctly
// suppressed the second drop attempt.
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

// A field that aliases a heap-allocated local of the same class type.
// Both the local's chain pop and the parent's auto-drop fire on the
// same address; the live-set claim ensures only one frees.
TEST(FieldOwnershipAliasingTests, classFieldAliasingHeapLocalDoesNotDoubleFree) {
    auto src =
        "package test;\n"
        "public class Leaf {\n"
        "    public int32 v;\n"
        "    public Leaf(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Holder {\n"
        "    public Leaf ref;\n"
        "    public Holder(Leaf r) { this.ref #= r; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Leaf leaf = heap Leaf(42);\n"
        "        Holder holder = heap Holder(leaf);\n"
        "        return leaf.v;\n"  // still live; both drop at scope exit
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Two field-resident aliases of the same heap local. Pre-Solution-B
// each auto-drop would fire on the same Leaf → triple-free with the
// local's chain pop on top.
TEST(FieldOwnershipAliasingTests, twoClassFieldsAliasingSameLocalDoNotDoubleFree) {
    auto src =
        "package test;\n"
        "public class Leaf {\n"
        "    public int32 v;\n"
        "    public Leaf(int32 v) { this.v = v; }\n"
        "}\n"
        "public class TwoSlots {\n"
        "    public Leaf a;\n"
        "    public Leaf b;\n"
        "    public TwoSlots(Leaf x) { this.a #= x; this.b #= x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Leaf leaf = heap Leaf(7);\n"
        "        TwoSlots p = heap TwoSlots(leaf);\n"
        "        return p.a.v + p.b.v + leaf.v;\n"  // 7+7+7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// Container with an inner scope holding the alias; the inner scope's
// drop fires first and the auto-drop walks through the alias. Outer
// scope's local must still pop cleanly (live-set marks the address
// gone after the first drop).
TEST(FieldOwnershipAliasingTests, aliasInInnerScopeDropsCleanly) {
    auto src =
        "package test;\n"
        "public class Leaf {\n"
        "    public int32 v;\n"
        "    public Leaf(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Holder {\n"
        "    public Leaf ref;\n"
        "    public Holder(Leaf r) { this.ref #= r; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Leaf leaf = heap Leaf(13);\n"
        "        {\n"
        "            Holder h = heap Holder(leaf);\n"
        "        }\n"  // Holder drops; its auto-drop frees leaf via live-set
        "        return 0;\n"  // leaf's outer-scope pop must no-op (already claimed)
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Mixed: a class with BOTH a sole-owner heap field (auto-drop fires for
// real) AND an aliased class field (auto-drop fires but the live-set
// claim no-ops). Verifies both paths coexist within one drop wrapper.
TEST(FieldOwnershipAliasingTests, mixedOwnerAndAliasFieldsDropCorrectly) {
    auto src =
        "package test;\n"
        "public class Owned {\n"
        "    public int32 v;\n"
        "    public Owned() { this.v = 100; }\n"
        "}\n"
        "public class Borrowed {\n"
        "    public int32 v;\n"
        "    public Borrowed(int32 v) { this.v = v; }\n"
        "}\n"
        "public class Container {\n"
        "    public Owned owned;\n"      // sole owner — auto-drop frees this
        "    public Borrowed alias;\n"   // aliased — auto-drop no-ops via live-set
        "    public Container(Borrowed b) {\n"
        "        this.owned = heap Owned();\n"
        "        this.alias #= b;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Borrowed b = heap Borrowed(50);\n"
        "        Container c = heap Container(b);\n"
        "        return c.owned.v + c.alias.v + b.v;\n"  // 100 + 50 + 50 = 200
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 200);
}

// Null alias field — auto-drop reads null and short-circuits without
// touching the live-set.
TEST(FieldOwnershipAliasingTests, nullAliasFieldDropsCleanly) {
    auto src =
        "package test;\n"
        "public class Other { }\n"
        "public class Container {\n"
        "    public Other o;\n"
        "    public Container() { }\n"  // o stays null
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Container c = heap Container();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Array field aliasing a heap-allocated local array. Pre-Solution-B
// the container's auto-drop and the local's chain pop both call
// __cajeta_free_array on the same buffer.
TEST(FieldOwnershipAliasingTests, arrayFieldAliasingLocalDoesNotDoubleFree) {
    auto src =
        "package test;\n"
        "public class BufferHolder {\n"
        "    public int32[] buf;\n"
        "    public BufferHolder(int32[] b) { this.buf #= b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = heap int32[4];\n"
        "        arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;\n"
        "        BufferHolder h = heap BufferHolder(arr);\n"
        "        return arr[0] + h.buf[1] + h.buf[2] + arr[3];\n"  // 1+2+3+4
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}
