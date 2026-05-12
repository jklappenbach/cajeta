//
// Smoke tests for the RTTI build pipeline.
//
// `CajetaClass::generatePrototype` runs `writeVirtualTable()` after method
// prototype generation; that triggers `StructureMetadata::populate` which
// builds both the vtable global AND the RTTI global. Each is named:
//
//   <canonical>#VTable      — function pointer table (one slot per virtual)
//   <canonical>#RttiGlobal  — class metadata: name, properties, methods, parents
//
// These tests verify the RTTI blob builds and the LLVM module verifies on a
// few common class shapes. Behavioral consumption (reflection / instanceof
// dispatching on RTTI) lands when there's a stdlib surface to hit.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.R");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Bare class -------------------------------------------------------------

TEST(RttiSmokeTests, bareClassCompiles) {
    // Default-constructor-only class. RTTI blob has empty property/method
    // lists (except the synthesized constructor), no parents. Verifies the
    // baseline path.
    auto src =
        "package test;\n"
        "public class Empty { }\n"
        "public final class R {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- Class with properties -------------------------------------------------

TEST(RttiSmokeTests, classWithPropertiesCompiles) {
    // Each property contributes its own (uniquely-typed) struct to the RTTI
    // properties tuple. Test verifies the RTTI build handles multiple
    // distinct property types.
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 width;\n"
        "    public int32 height;\n"
        "    public int64 area;\n"
        "}\n"
        "public final class R {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- Class with methods (mix of static + non-static + with parameters) ----

TEST(RttiSmokeTests, classWithMethodsCompiles) {
    // Method metadata includes a per-method nested struct of parameter
    // metadata. With a `this` parameter inserted for non-static methods,
    // the parameter struct has at least one entry; static methods have
    // zero (or whatever the user declared).
    auto src =
        "package test;\n"
        "public class Service {\n"
        "    public int32 plain() { return 1; }\n"
        "    public int32 withArg(int32 x) { return x; }\n"
        "    public int32 twoArgs(int32 a, int32 b) { return a + b; }\n"
        "    public static int32 staticHelper() { return 99; }\n"
        "}\n"
        "public final class R {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- Class with annotations ------------------------------------------------

TEST(RttiSmokeTests, annotatedClassCompiles) {
    // Class-level annotations contribute strings to the class-annotation
    // tuple in the RTTI blob.
    auto src =
        "package test;\n"
        "@Final\n"
        "public class Tagged {\n"
        "    public int32 x;\n"
        "}\n"
        "public final class R {\n"
        "    public static int32 run() { return 3; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// --- Multiple classes coexist ---------------------------------------------

TEST(RttiSmokeTests, multipleClassesGetIndependentRtti) {
    // Each class produces its own `<canonical>#RttiGlobal`. If `populate`
    // shared state across classes (it shouldn't), the second class's blob
    // would inherit the first's leftover types.
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 foo() { return 1; }\n"
        "}\n"
        "public class B {\n"
        "    public int32 bar;\n"
        "    public int32 baz() { return 2; }\n"
        "}\n"
        "public final class R {\n"
        "    public static int32 run() { return 42; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
