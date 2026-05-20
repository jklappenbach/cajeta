//
// Regression tests for the l-value-coercion gap in VariableInitializer that
// surfaced during the L2 lambdas work. Before the fix, initializing a local
// from another l-value expression — `int32 a = b;`, `int32 v = arr[i];`,
// `int32 f = obj.x;` — silently stored the SLOT POINTER into the destination,
// not the loaded value. Reading the local would then return garbage (or
// segfault when the slot was a struct or array header).
//
// Fix routes the initializer's value through loadIfLValue so an l-value
// initializer always produces an r-value before the store.
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

// `int32 a = b;` — copy from another local. Pre-fix this stored the
// alloca pointer into a's slot.
TEST(LocalInitializerTests, copyFromAnotherLocalPrimitive) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 b = 42;\n"
        "        int32 a = b;\n"
        "        return a;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `int32 v = arr[i];` — read an array element into a fresh local. Pre-fix
// segfaulted on garbage stored into v's slot.
TEST(LocalInitializerTests, copyFromArrayIndex) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] arr = new int32[3];\n"
        "        arr[1] = 17;\n"
        "        int32 v = arr[1];\n"
        "        return v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// Chain of l-value initializations exercises the load path repeatedly.
TEST(LocalInitializerTests, chainedCopiesPreserveValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 7;\n"
        "        int32 b = a;\n"
        "        int32 c = b;\n"
        "        int32 d = c;\n"
        "        return d;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Widening cast happens after the load, not against the slot pointer. Pre-fix
// this would have stored a ptr into an i64 slot and produced garbage.
TEST(LocalInitializerTests, widenedCopyAcrossWidths) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 a = 100;\n"
        "        int64 wide = a;\n"
        "        return (int32) wide;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

// String is a pointer-typed local; initializing one String from another
// should copy the pointer, not the alloca's address.
TEST(LocalInitializerTests, copyFromAnotherLocalString) {
    // `t.count()` is the canonical codepoint count post-2026-05-18
    // naming shift. `t.length()` is intentionally absent on the class
    // (see runtime/src/cajeta/lang/String.cajeta § 191 — "ambiguous in
    // practice"); `count()` is the migrated equivalent.
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String s = \"hello\";\n"
        "        String t = s;\n"
        "        return (int32) t.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
