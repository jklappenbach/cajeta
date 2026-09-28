// A static call through a type parameter, `T.m()`, resolves to the bound type's
// static method in each monomorphized body.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

TEST(TypeParameterStaticCallTests, staticCallThroughTypeParameter) {
    auto src =
        "package test;\n"
        "public class Seven { public static int32 value() { return 7; } }\n"
        "public class Nine { public static int32 value() { return 9; } }\n"
        "public class Box<T> {\n"
        "    public Box() { return; }\n"
        "    public int32 make() { return T.value(); }\n"
        "}\n"
        "public final class U {\n"
        "    public static int32 run() {\n"
        "        Box<Seven> a = heap Box<Seven>();\n"
        "        Box<Nine> b = heap Box<Nine>();\n"
        "        return a.make() * 10 + b.make();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 79);
}

TEST(TypeParameterStaticCallTests, staticCallThroughMethodTypeParameter) {
    auto src =
        "package test;\n"
        "public class Seven { public static int32 value() { return 7; } }\n"
        "public class Nine { public static int32 value() { return 9; } }\n"
        "public final class U {\n"
        "    static int32 read<T>(T witness) { return T.value(); }\n"
        "    public static int32 run() {\n"
        "        Seven s = heap Seven();\n"
        "        Nine n = heap Nine();\n"
        "        return U.read(s) * 10 + U.read(n);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.U");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 79);
}
