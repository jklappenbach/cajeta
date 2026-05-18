// Tests for passing `this` as an argument to another class's constructor.
//
// Pre-fix, `loadIfLValue`'s catch-all loaded class-typed values
// through `v` using `resolved->getLlvmType()` (the inline struct
// shape). For a class-typed alloca slot — which holds a `ptr` per
// the class-pass-by-pointer rule — that load produced the inline
// struct instead of the ptr, mismatching ctor signatures that
// expected `ptr`. JIT verifier rejected with "Call parameter type
// does not match function signature!".
//
// Fix: in loadIfLValue's catch-all, when the resolved type is a
// plain CajetaClass (not view, not interface) and `v` is already a
// `ptr`, load through `v` as `ptr` (not as the inline struct).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// `this` passed as a ctor arg — the failing baseline before the fix.
TEST(ThisAsArgTests, thisAsCtorArg) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "    public #Wrapper wrap() { return heap Wrapper(this); }\n"
        "}\n"
        "public class Wrapper {\n"
        "    public Box inner;\n"
        "    public Wrapper(Box b) { this.inner = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(42);\n"
        "        Wrapper w = b.wrap();\n"
        "        return w.inner.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// `this` passed to a method (not just ctor).
TEST(ThisAsArgTests, thisAsMethodArg) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "    public int32 reveal() { return Reader.read(this); }\n"
        "}\n"
        "public class Reader {\n"
        "    public static int32 read(Box b) { return b.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(7);\n"
        "        return b.reveal();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Chained: pass `this` through two layers.
TEST(ThisAsArgTests, thisChainedThroughTwoLayers) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "    public #Outer wrap() { return heap Outer(this); }\n"
        "}\n"
        "public class Inner {\n"
        "    public Box b;\n"
        "    public Inner(Box b) { this.b = b; }\n"
        "}\n"
        "public class Outer {\n"
        "    public Inner i;\n"
        "    public Outer(Box b) { this.i = heap Inner(b); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(11);\n"
        "        Outer o = b.wrap();\n"
        "        return o.i.b.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}
