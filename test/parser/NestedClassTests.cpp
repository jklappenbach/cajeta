// Nested class tests. The grammar has always accepted classDeclaration
// inside classBody (CajetaParser.g4 line 187); what was missing was
// visitor wiring — visitClassBodyDeclaration tried to cast the
// CajetaClassPtr returned by visitClassDeclaration into a
// MemberDeclarationPtr and tripped any_cast. Adding NestedClassDeclaration
// (a no-op MemberDeclaration wrapper) bridges the gap.
//
// v1 supports static-nested classes (Java-style `public static class
// Inner { ... }`). No implicit outer-this reference — the nested class
// is an independent type that happens to live under the outer's
// canonical namespace (`Outer.Inner`).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Just declaring a nested class compiles cleanly.
TEST(NestedClassTests, declareNestedCompiles) {
    auto src =
        "package test;\n"
        "public class Outer {\n"
        "    public static class Inner {\n"
        "        public int32 v;\n"
        "        public Inner(int32 x) { this.v = x; }\n"
        "    }\n"
        "    public int32 n;\n"
        "    public Outer() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Use the nested type at a use site via `Outer.Inner`.
TEST(NestedClassTests, instantiateNestedType) {
    auto src =
        "package test;\n"
        "public class Outer {\n"
        "    public static class Inner {\n"
        "        public int32 v;\n"
        "        public Inner(int32 x) { this.v = x; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Outer.Inner i = heap Outer.Inner(42);\n"
        "        return i.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Nested class with multiple methods.
TEST(NestedClassTests, nestedWithMethods) {
    auto src =
        "package test;\n"
        "public class Outer {\n"
        "    public static class Counter {\n"
        "        public int32 n;\n"
        "        public Counter() { this.n = 0; }\n"
        "        public void inc() { this.n = this.n + 1; }\n"
        "        public int32 get() { return this.n; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Outer.Counter c = heap Outer.Counter();\n"
        "        c.inc();\n"
        "        c.inc();\n"
        "        c.inc();\n"
        "        return c.get();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// Two nested classes in the same outer.
TEST(NestedClassTests, multipleNestedClasses) {
    auto src =
        "package test;\n"
        "public class Pair {\n"
        "    public static class Left {\n"
        "        public int32 v;\n"
        "        public Left(int32 x) { this.v = x; }\n"
        "    }\n"
        "    public static class Right {\n"
        "        public int32 v;\n"
        "        public Right(int32 x) { this.v = x; }\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pair.Left  l = heap Pair.Left(7);\n"
        "        Pair.Right r = heap Pair.Right(13);\n"
        "        return l.v + r.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 20);
}

// Outer class instance + nested class instance live independently.
TEST(NestedClassTests, outerAndNestedCoexist) {
    auto src =
        "package test;\n"
        "public class Container {\n"
        "    public static class Item {\n"
        "        public int32 value;\n"
        "        public Item(int32 v) { this.value = v; }\n"
        "    }\n"
        "    public int32 capacity;\n"
        "    public Container(int32 cap) { this.capacity = cap; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Container c = heap Container(100);\n"
        "        Container.Item i = heap Container.Item(42);\n"
        "        return c.capacity + i.value;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 142);
}
