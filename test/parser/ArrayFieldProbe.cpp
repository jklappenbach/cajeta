// Probes for array-typed class fields. Companion to the layout fix
// in CajetaClass::generatePrototype's fieldLayoutType: array fields
// are stored as pointers to heap headers, not embedded inline.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
using cajeta_test::CajetaJit;

TEST(ArrayFieldProbe, concreteIntArrayAsClassField) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32[] xs;\n"
        "    public Box() {\n"
        "        this.xs = new int32[3];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = new Box();\n"
        "        b.xs[0] = 7;\n"
        "        b.xs[1] = 11;\n"
        "        b.xs[2] = 13;\n"
        "        return b.xs[0] + b.xs[1] + b.xs[2];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 31);
}

// Generic T[] field on a templated class. Exercises the full
// template-instantiation path: the field is laid out with T's bound
// type after instantiation, the constructor's `new T[N]` allocates
// against the bound type, and `c.data[i]` indexing reads through the
// instantiated array.
TEST(ArrayFieldProbe, genericArrayAsClassField) {
    auto src =
        "package test;\n"
        "public class Container<T> {\n"
        "    public T[] data;\n"
        "    public Container() {\n"
        "        this.data = new T[2];\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Container<int32> c = new Container<int32>();\n"
        "        c.data[0] = 100;\n"
        "        c.data[1] = 200;\n"
        "        return c.data[0] + c.data[1];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 300);
}
