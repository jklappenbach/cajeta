// Tests for the @ToString Lombok-mirror synthesizer
// (cajeta-docs/stdlib/Annotations.md § @ToString). v1 ships the
// PROPERTIES format only: `ClassName(field1=v1,field2=v2,...)`.
//
// Each test compiles a Cajeta source and calls a `static char* run()`
// that invokes toString() on a heap instance and returns the result.
// The C++ side compares against expected strings.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <cstring>
#include <string>

using cajeta_test::CajetaJit;

namespace {
    std::string runToString(const std::string& src) {
        auto jit = CajetaJit::compile(src, "test.D");
        auto fn = jit->lookup<const char* (*)()>("run");
        const char* r = fn();
        return r ? std::string(r) : std::string("<null>");
    }
}

// Single int field — the simplest possible case.
TEST(ToStringTests, singleIntField) {
    auto src =
        "package test;\n"
        "@ToString public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(42);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "P(n=42)");
}

// Multiple primitive fields — exercises the comma separator + multi-width handling.
TEST(ToStringTests, multipleIntFields) {
    auto src =
        "package test;\n"
        "@ToString public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point(int32 a, int32 b) { this.x = a; this.y = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Point p = heap Point(3, 7);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Point(x=3,y=7)");
}

// Mixed primitive widths.
TEST(ToStringTests, mixedPrimitiveWidths) {
    auto src =
        "package test;\n"
        "@ToString public class M {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "    public M(int8 a, int16 b, int32 c, int64 d) {\n"
        "        this.a = a; this.b = b; this.c = c; this.d = d;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        M m = heap M(1, 2, 3, 4);\n"
        "        return m.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "M(a=1,b=2,c=3,d=4)");
}

// Boolean field.
TEST(ToStringTests, booleanField) {
    auto src =
        "package test;\n"
        "@ToString public class Flag {\n"
        "    public boolean on;\n"
        "    public Flag(boolean v) { this.on = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Flag f = heap Flag(true);\n"
        "        return f.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Flag(on=true)");
}

// Float field — uses %g formatting.
TEST(ToStringTests, floatField) {
    auto src =
        "package test;\n"
        "@ToString public class Measure {\n"
        "    public float64 ratio;\n"
        "    public Measure(float64 r) { this.ratio = r; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Measure m = heap Measure(0.5);\n"
        "        return m.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Measure(ratio=0.5)");
}

// @ToString.Exclude omits the field.
TEST(ToStringTests, excludeFieldOmitted) {
    auto src =
        "package test;\n"
        "@ToString public class Secret {\n"
        "    public int32 visible;\n"
        "    @Exclude public int32 hidden;\n"
        "    public Secret(int32 v, int32 h) { this.visible = v; this.hidden = h; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Secret s = heap Secret(1, 99);\n"
        "        return s.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Secret(visible=1)");
}

// Static fields skipped automatically.
TEST(ToStringTests, staticFieldsSkipped) {
    auto src =
        "package test;\n"
        "@ToString public class Counter {\n"
        "    public static int32 total = 999;\n"
        "    public int32 n;\n"
        "    public Counter(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Counter c = heap Counter(5);\n"
        "        return c.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Counter(n=5)");
}

// Empty class (no instance fields) still produces a valid toString.
TEST(ToStringTests, emptyClass) {
    auto src =
        "package test;\n"
        "@ToString public class Empty {\n"
        "    public Empty() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Empty e = heap Empty();\n"
        "        return e.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Empty()");
}

// User-declared toString() wins — synthesizer skips.
TEST(ToStringTests, userToStringWins) {
    auto src =
        "package test;\n"
        "@ToString public class Custom {\n"
        "    public int32 n;\n"
        "    public Custom(int32 v) { this.n = v; }\n"
        "    public String toString() { return \"custom\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Custom c = heap Custom(7);\n"
        "        return c.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "custom");
}

// Negative ints rendered correctly.
TEST(ToStringTests, negativeInt) {
    auto src =
        "package test;\n"
        "@ToString public class Neg {\n"
        "    public int32 n;\n"
        "    public Neg(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Neg n = heap Neg(-7);\n"
        "        return n.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Neg(n=-7)");
}

// TO_STRING_JSON format is deferred — should throw.
TEST(ToStringTests, jsonFormatDeferred) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class P {\n"
        "    public int32 n;\n"
        "    public P() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_JSON_NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_JSON_NOT_IMPLEMENTED");
    }
}

// Sanity: no @ToString → no synthesis (so calling toString() goes to
// whatever Object provides, currently a null stub — just verify the
// class compiles).
TEST(ToStringTests, noAnnotationCompilesNormally) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 n;\n"
        "    public Plain(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain p = heap Plain(5);\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}
