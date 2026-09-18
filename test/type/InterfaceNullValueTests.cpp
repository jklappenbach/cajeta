//
// InterfaceNullValueTests — a null value at an interface-typed return.
//
// An interface travels as a 24-byte { data, vtable, kind } body, and a local
// of interface type holds a POINTER to one. Null has to survive both: the
// return path wraps a concrete class into a body, but a null has no class to
// take a vtable from, so it fell through to a by-value aggregate load that
// read the body off address zero. It faulted whether the null came from a
// literal or from an interface local, and whether the caller kept the result
// or discarded it — a method returning `null` for an interface simply could
// not be called.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>
using cajeta_test::CajetaJit;
namespace {
int run(const std::string& body) {
    std::string program =
        "package test;\n"
        "public interface Ifc { int32 id(); }\n"
        "public final class Impl implements Ifc {\n"
        "    public Impl() { return; }\n"
        "    public int32 id() { return 7; }\n"
        "}\n"
        "public final class Box {\n"
        "    Impl held;\n"
        "    public Box(Impl x) { this.held #= x; }\n"
        "    public Ifc give(boolean yes) {\n"
        "        if (yes) { return this.held; }\n"
        "        return null;\n"
        "    }\n"
        "    public Ifc giveViaLocal(boolean yes) {\n"
        "        Ifc none = null;\n"
        "        if (yes) { return this.held; }\n"
        "        return none;\n"
        "    }\n"
        "}\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        Impl i = heap Impl();\n"
        "        Box b = heap Box(i);\n"
        + body +
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.T", o);
    if (!jit) return -100;
    auto fn = jit->lookup<int (*)()>("run");
    if (!fn) return -101;
    return fn();
}
}
TEST(InterfaceNullValue, localHoldingNullComparesEqual) {
    EXPECT_EQ(run("        Ifc a = b.give(false);\n"
                  "        if (a == null) { return 0; }\n"
                  "        return 1;\n"), 0);
}
TEST(InterfaceNullValue, inlineNullCallResultComparesAgainstNull) {
    EXPECT_EQ(run("        if (b.give(false) == null) { return 0; }\n"
                  "        return 1;\n"), 0);
}
TEST(InterfaceNullValue, inlineNonNullCallResultComparesAgainstNull) {
    EXPECT_EQ(run("        if (b.give(true) != null) { return 0; }\n"
                  "        return 1;\n"), 0);
}
TEST(InterfaceNullValue, localHoldingNonNullIsNotNullAndCallsThrough) {
    EXPECT_EQ(run("        Ifc a = b.give(true);\n"
                  "        if (a == null) { return 1; }\n"
                  "        if (a.id() != 7) { return 2; }\n"
                  "        return 0;\n"), 0);
}

TEST(InterfaceNullValue, nullIsStorableWhenNeverCompared) {
    EXPECT_EQ(run("        Ifc a = b.give(false);\n"
                  "        return 0;\n"), 0);
}
TEST(InterfaceNullValue, aNullLiteralInAnInterfaceLocalComparesEqual) {
    EXPECT_EQ(run("        Ifc a = null;\n"
                  "        if (a == null) { return 0; }\n"
                  "        return 1;\n"), 0);
}
TEST(InterfaceNullValue, aClassTypedNullComparesEqual) {
    EXPECT_EQ(run("        Impl a = null;\n"
                  "        if (a == null) { return 0; }\n"
                  "        return 1;\n"), 0);
}

TEST(InterfaceNullValue, callingForNullAndDiscardingTheResult) {
    EXPECT_EQ(run("        b.give(false);\n"
                  "        return 0;\n"), 0);
}
TEST(InterfaceNullValue, returningANullInterfaceLocal) {
    EXPECT_EQ(run("        Ifc a = b.giveViaLocal(false);\n"
                  "        return 0;\n"), 0);
}
