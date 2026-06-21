//
// Fixed-size inline array fields, general over the element type T (SSO plan
// Unit 4; spec 2.1.6). The inline layout/access/drop must use the element's
// own LLVM size and shape — not assume int8/4-byte elements. Covers a non-int
// primitive (float64) and a @ValueType struct element laid out inline.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

// 4.1.1 — `float64[3]` inline field round-trips (8-byte element, not int8).
TEST(InlineArrayFieldGeneralTests, Float64ArrayRoundTrip) {
    std::string src =
        "package test;\n"
        "public final class V {\n"
        "    public float64[3] v;\n"
        "    public V() {}\n"
        "    public float64 run2() {\n"
        "        this.v[0] = 1.5;\n"
        "        this.v[1] = 2.25;\n"
        "        this.v[2] = 4.0;\n"
        "        return this.v[0] + this.v[1] + this.v[2];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static float64 run() {\n"
        "        V x = heap V();\n"
        "        return x.run2();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<double (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_DOUBLE_EQ(fn(), 7.75);
}

// 4.1.2 — a @ValueType struct element (`Point[4]`) lays out inline at
// i * sizeof(Point); write/read `pts[i].x` / `pts[i].y` with correct striding.
TEST(InlineArrayFieldGeneralTests, ValueTypeStructArrayInlineStriding) {
    std::string src =
        "package test;\n"
        "@ValueType public final class Point {\n"
        "    public float64 x;\n"
        "    public float64 y;\n"
        "    public Point(float64 x, float64 y) { this.x = x; this.y = y; }\n"
        "}\n"
        "public final class Grid {\n"
        "    public Point[4] pts;\n"
        "    public Grid() {}\n"
        "    public float64 run2() {\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            this.pts[i].x = (float64)(i + 1);\n"
        "            this.pts[i].y = (float64)(i * 10);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float64 sum = 0.0;\n"
        "        i = 0;\n"
        "        while (i < 4) { sum = sum + this.pts[i].x + this.pts[i].y; i = i + 1; }\n"
        "        return sum;\n" // (1+2+3+4) + (0+10+20+30) = 10 + 60 = 70
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static float64 run() {\n"
        "        Grid g = heap Grid();\n"
        "        return g.run2();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<double (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_DOUBLE_EQ(fn(), 70.0);
}
