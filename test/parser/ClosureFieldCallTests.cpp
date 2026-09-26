// Calling a closure-typed field directly, `recv.fn(args)`, must invoke the closure
// with its arguments lowered as values, for void and value closures, for any
// receiver class visibility, and whether the closure was lent or moved in.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "class Bx {\n"
    "    public int32 v;\n"
    "    public Bx(int32 v) { this.v = v; }\n"
    "}\n"
    "class H {\n"
    "    (Bx, Bx) -> void fn;\n"
    "    (Bx) -> int32 val;\n"
    "    Bx a;\n"
    "    Bx b;\n"
    "    public H((Bx, Bx) -> void f, (Bx) -> int32 g) {\n"
    "        this.fn #= f;\n"
    "        this.val #= g;\n"
    "        this.a = heap Bx(3);\n"
    "        this.b = heap Bx(4);\n"
    "    }\n"
    "    public int32 voidFields() {\n"
    "        this.fn(this.a, this.b);\n"
    "        return this.a.v;\n"
    "    }\n"
    "    public int32 voidLocals() {\n"
    "        Bx x = this.a;\n"
    "        Bx y = this.b;\n"
    "        this.fn(x, y);\n"
    "        return this.a.v;\n"
    "    }\n"
    "    public int32 voidViaLocal() {\n"
    "        (Bx, Bx) -> void k = this.fn;\n"
    "        k(this.a, this.b);\n"
    "        return this.a.v;\n"
    "    }\n"
    "    public int32 valFields() {\n"
    "        return this.val(this.a);\n"
    "    }\n"
    "}\n";

int32_t runI32(const std::string& body) {
    std::string src = std::string(kPrelude)
        + "public class S {\n"
          "    public static int32 run() {\n"
          "        (Bx, Bx) -> void f = (Bx x, Bx y) -> { x.v = x.v + y.v; };\n"
          "        (Bx) -> int32 g = (Bx x) -> x.v * 10;\n"
        + body
        + "    }\n"
          "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

TEST(ClosureFieldCallTests, voidFieldWithFieldArgumentsRuns) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        return h.voidFields();\n"), 7);
}

TEST(ClosureFieldCallTests, voidFieldWithFieldArgumentsRunsTwice) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        h.voidFields();\n"
                     "        return h.voidFields();\n"), 11);
}

TEST(ClosureFieldCallTests, voidFieldFromOutsideWithFieldArguments) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        h.fn(h.a, h.b);\n"
                     "        return h.a.v;\n"), 7);
}

TEST(ClosureFieldCallTests, voidFieldWithLocalArgumentsRuns) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        return h.voidLocals();\n"), 7);
}

TEST(ClosureFieldCallTests, voidFieldCopiedToLocalRuns) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        return h.voidViaLocal();\n"), 7);
}

TEST(ClosureFieldCallTests, valueFieldOnNonPublicClassWithFieldArgumentMoved) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        return h.val(h.a);\n"), 30);
}

TEST(ClosureFieldCallTests, valueFieldOnNonPublicClassWithFieldArgumentLent) {
    EXPECT_EQ(runI32("        H h = heap H(f, g);\n"
                     "        return h.val(h.a);\n"), 30);
}

TEST(ClosureFieldCallTests, valueFieldThroughThisWithFieldArgument) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        return h.valFields();\n"), 30);
}

TEST(ClosureFieldCallTests, valueFieldWithLocalArgument) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        Bx z = heap Bx(5);\n"
                     "        return h.val(z);\n"), 50);
}

TEST(ClosureFieldCallTests, valueFieldCopiedToLocal) {
    EXPECT_EQ(runI32("        H h = heap H(#f, #g);\n"
                     "        (Bx) -> int32 k = h.val;\n"
                     "        return k(h.a);\n"), 30);
}

TEST(ClosureFieldCallTests, valueFieldOnPublicClassDirect) {
    std::string src =
        "package test;\n"
        "class Bx {\n"
        "    public int32 v;\n"
        "    public Bx(int32 v) { this.v = v; }\n"
        "}\n"
        "public class S {\n"
        "    (Bx) -> int32 val;\n"
        "    public Bx a;\n"
        "    public S((Bx) -> int32 g) {\n"
        "        this.val #= g;\n"
        "        this.a = heap Bx(6);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        (Bx) -> int32 g = (Bx x) -> x.v * 10;\n"
        "        S s = heap S(#g);\n"
        "        Bx z = heap Bx(2);\n"
        "        return s.val(z) + s.val(s.a);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 80);
}
