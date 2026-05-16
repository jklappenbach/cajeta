//
// Session 10 — struct/class → interface assignment + ownership.
//
// Covers the new functionality unlocked by the S9.5 fat-pointer
// foundation: assigning struct values to interface variables
// (BORROWED_STRUCT kind), the `#` operator for owned class→iface
// (OWNED_CLASS), the borrow tracker's struct-rooted iface escape
// rejection, and the kind-tag drop chain.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/type/CajetaClass.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// ---------------------------------------------------------------------
// S10.1 — struct → interface assignment.
//
// Assigning a struct value to an interface-typed local builds a fat
// pointer with kind = IFACE_KIND_BORROWED_STRUCT, data pointing at the
// struct's stack-resident body, and vtable pointing at the per-(struct,
// iface) global synthesized by S9.2. Dispatch through the interface
// value lands on the struct's concrete method.
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, structToInterfaceDispatches) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hi h = Hi { base: 10 };\n"
        "        Greeter g = h;\n"  // struct → interface
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// ---------------------------------------------------------------------
// S10.2 — class → interface assignment (borrowed). The default
// without `#` produces a BORROWED_CLASS fat pointer; dispatch lands
// on the class's implementation, and the kind-tag drop chain no-ops
// at scope exit (the underlying Hello leaks in v1 because there's no
// fresh-allocation owner to free it; tests for the OWNED variant
// below pin the drop-on-scope-exit behavior).
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, classToInterfaceBorrowedDispatches) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 42; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hello h = new Hello();\n"
        "        Greeter g = h;\n"
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// S10.2 — `#h` class → interface assignment (OWNED_CLASS). The
// MoveExpression wrapper signals ownership transfer: the source local
// gets marked moved + its class-instance drop entry deactivated, and
// the interface local's kind tag becomes OWNED_CLASS. Dispatch still
// lands correctly; the underlying Hello cleans up when g goes out of
// scope (S10.4 — verified via drop-count test below).
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, classToInterfaceOwnedDispatches) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 99; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hello h = new Hello();\n"
        "        Greeter g = #h;\n"  // ownership transfer
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// ---------------------------------------------------------------------
// S10.3 — struct → interface escape rejection. The borrow tracker
// catches returning an interface local whose data ptr roots in a
// function-local struct; the returned fat pointer would dangle past
// the frame's death. CAJETA_ERROR_INTERFACE_VALUE_ESCAPE points the
// user at boxing the underlying data in a class.
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, structInterfaceEscapeRejected) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base; }\n"
        "}\n"
        "public final class S {\n"
        "    public static Greeter leak() {\n"
        "        Hi h = Hi { base: 5 };\n"
        "        Greeter g = h;\n"
        "        return g;\n"  // would dangle
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_INTERFACE_VALUE_ESCAPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_INTERFACE_VALUE_ESCAPE");
    }
}

// ---------------------------------------------------------------------
// S10.4 — drop count verification across kinds. BORROWED_* values
// no-op at scope exit; OWNED_CLASS invokes the underlying class's
// drop. Read __cajeta_drop_count from the native runtime to confirm.
// ---------------------------------------------------------------------

// Drop counts must be observed from within the JIT-compiled program
// because the JIT's runtime maintains its own counter (the embedded
// bitcode in the compiled module has its own copy of the runtime's
// static state, separate from whatever's linked into the test binary).
// `Cajeta.dropCount()` and `Cajeta.dropCountReset()` route to the
// JIT-side counter; the C-side `__cajeta_drop_count_get` would read
// the wrong instance and stay at 0.

// BORROWED_CLASS: dispatch through interface, then both g and h
// go out of scope. h's class-instance drop entry fires once
// (the actual Hello drop), g's iface_drop helper reads BORROWED
// and no-ops at the drop fn level — but both ENTRIES still pop
// from the chain. __cajeta_drop_pop_run increments the counter
// once per active entry that runs its drop fn (regardless of
// whether the drop fn does meaningful work). For BORROWED_CLASS:
// h's entry is active + runs class drop → +1; g's entry is active
// + runs iface_drop helper (no-ops on BORROWED) → +1. Total 2.
TEST(StructInterfaceDispatchTests, borrowedClassInterfaceDropAccounting) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 inner() {\n"
        "        Hello h = new Hello();\n"
        "        Greeter g = h;\n"
        "        return g.greet();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        inner();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// OWNED_CLASS: source class local's drop is deactivated by `#`,
// so h's entry pops but is INACTIVE (no counter bump). g's iface
// drop entry pops, is active, fires the helper, which dispatches
// OWNED_CLASS and calls Hello's drop on data → +1 (the helper
// invocation increments the counter via __cajeta_drop_pop_run).
// Net: 1 bump.
TEST(StructInterfaceDispatchTests, ownedClassInterfaceDropAccounting) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 inner() {\n"
        "        Hello h = new Hello();\n"
        "        Greeter g = #h;\n"
        "        return g.greet();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        inner();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// BORROWED_STRUCT: struct local's drop entry pops + runs (no owned
// class refs → drop walk is a no-op, but the pop still bumps the
// counter once). g's iface drop pops + runs the helper (BORROWED
// → no-op, but still +1). Net 2.
TEST(StructInterfaceDispatchTests, borrowedStructInterfaceDropAccounting) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 inner() {\n"
        "        Hi h = Hi { base: 7 };\n"
        "        Greeter g = h;\n"
        "        return g.greet();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        inner();\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// ---------------------------------------------------------------------
// S10.5 — layout-size + kind-tag values match the spec. These are
// static asserts in the C bridge that protect the IR-side layout
// from drifting silently. If the fat pointer ever changes shape or
// the enum constants ever renumber, this test catches it before any
// runtime test would dispatch through a broken layout.
// ---------------------------------------------------------------------

TEST(StructInterfaceDispatchTests, layoutAndKindTagInvariants) {
    // Fat-pointer body is { ptr data, ptr vtable, i64 kind } = 24 bytes.
    EXPECT_EQ(cajeta::IFACE_FAT_POINTER_BYTES, 24u);
    EXPECT_EQ((int64_t) cajeta::IFACE_KIND_BORROWED_CLASS, 0);
    EXPECT_EQ((int64_t) cajeta::IFACE_KIND_OWNED_CLASS, 1);
    EXPECT_EQ((int64_t) cajeta::IFACE_KIND_BORROWED_STRUCT, 2);
}
