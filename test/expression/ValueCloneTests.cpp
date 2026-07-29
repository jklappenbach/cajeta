//
// element-ownership Unit 6 — clone() stabilization (spec §6.1.3-5). The
// reference-type half went live with the slices plan (ObjectCloneTests:
// RTTI-walk shallow copy, String-field stakes, override dispatch). This file
// pins the element-ownership requirements ON TOP: identity distinctness, a
// never-consumed receiver, and the VALUE-TYPE side — POD clone as plain value
// copy, shared-capable value clone via the COW copy hooks.
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

// 6.1.1 — reference type: clone() yields a DISTINCT heap object (mutating the
// clone leaves the source untouched; a plain assignment aliases).
TEST(ValueCloneTests, referenceCloneIsDistinctObject) {
    auto src = std::string(
        "package test;\n"
        "public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(10);\n"
        "        Cell b = (Cell) a.clone();\n"
        "        b.v = 99;\n"
        "        Cell c = a;\n"          // alias, not a copy
        "        c.v = 55;\n"
        "        return a.v;\n"           // 55: alias wrote through, clone didn't
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 55);
}

// 6.1.3 — clone() never consumes its receiver: the source stays usable (and
// droppable) after the call.
TEST(ValueCloneTests, cloneDoesNotConsumeReceiver) {
    auto src = std::string(
        "package test;\n"
        "public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(7);\n"
        "        Cell b = (Cell) a.clone();\n"
        "        return a.v + b.v;\n"     // 14 — both alive
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 14);
}

// 6.1.4 — POD value type (record of primitives): clone() is a plain value
// copy, independent storage.
TEST(ValueCloneTests, podRecordCloneIsValueCopy) {
    auto src = std::string(
        "package test;\n"
        "public record Pt { float64 x; float64 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pt a = Pt { x: 3.0, y: 4.0 };\n"
        "        Pt b = a.clone();\n"
        "        return (int32)(a.x + b.y);\n"  // 7
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 7);
}

// 6.3.1 — clone() satisfies an OWNING position directly: the duplicate is a
// fresh rvalue, so `owned.put(a.clone())` needs no `#` (the explicit-copy fix
// the TRANSFER_REQUIRED diagnostic names).
TEST(ValueCloneTests, cloneSatisfiesOwningPosition) {
    auto src = std::string(
        "package test;\n"
        "public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }\n"
        "public class Crate<K> {\n"
        "    public K store;\n"
        "    public void put(#K k) { this.store #= k; }\n"
        "    public K peek() { return this.store; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Crate<Cell> owned = heap Crate<Cell>();\n"
        "        Cell a = heap Cell(21);\n"
        "        owned.put(#((Cell) a.clone()));\n"   // duplicate surrenders (rev 2)
        "        return owned.peek().v + a.v;\n"    // 42 — source untouched
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 42);
}

// 6.1.2 / 6.2.2 — shared-capable value type (record with Utf8 payload):
// clone() returns a usable independent value whose heap payload copies via
// the COW value-copy hook (retain, not byte copy) — the sharedPopulation
// balance nets to zero once both drop.
TEST(ValueCloneTests, sharedCapableRecordCloneWorks) {
    auto src = std::string(
        "package test;\n"
        "public record Tag { Utf8 name; int32 n; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 before = Cajeta.sharedPopulation();\n"
        "        int32 got = 0;\n"
        "        {\n"
        "            Tag a = Tag { name: \"alpha\", n: 5 };\n"
        "            Tag b = a.clone();\n"
        "            got = a.n + b.n;\n"          // 10
        "        }\n"
        "        int64 after = Cajeta.sharedPopulation();\n"
        "        if (after != before) { return -1; }\n"  // balanced retain/release
        "        return got;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 10);
}
