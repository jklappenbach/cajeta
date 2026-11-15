// specs/instanceof-call-lhs: `call() instanceof T` folded to false; it must ask the runtime as a local does.

#include <gtest/gtest.h>

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

const char* TYPES =
    "import cajeta.collection.ArrayList;\n"
    "public interface Shape {\n"
    "    int32 sides();\n"
    "}\n"
    "public class Base {\n"
    "    public int32 tag;\n"
    "    public Base() { this.tag = 1; }\n"
    "}\n"
    "public class Lin extends Base implements Shape {\n"
    "    public Lin() { this.tag = 7; }\n"
    "    public int32 sides() { return 4; }\n"
    "}\n"
    "public class Holder {\n"
    "    public ArrayList<Base> xs;\n"
    "    public int32 calls;\n"
    "    public Lin only;\n"
    "    public Holder() {\n"
    "        this.xs #= heap ArrayList<Base>();\n"
    "        this.calls = 0;\n"
    "        this.only #= heap Lin();\n"
    "    }\n"
    "    public Base at(int64 i) { return this.xs.get(i); }\n"
    "    public Base next() { this.calls = this.calls + 1; return this.xs.get(0L); }\n"
    "    public Shape shape() { return this.only; }\n"
    "    public Lin lin() { return this.only; }\n"
    "    public Object any(int64 i) { return this.xs.get(i); }\n"
    "}\n";

std::string program(const std::string& body) {
    return std::string("package test;\n") + TYPES +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.xs.add(#heap Lin());\n"
        "        h.xs.add(#heap Base());\n" + body +
        "    }\n"
        "}\n";
}

} // namespace

// 3.1: a class-typed call result tests its runtime class, a subclass true and the base false.
TEST(InstanceofCallLhsTests, aCallResultAsksTheRuntime) {
    EXPECT_EQ(runI32(program(
        "        if (h.at(0L) instanceof Lin) {\n"
        "        } else { return -1; }\n"
        "        if (h.at(1L) instanceof Lin) { return -2; }\n"
        "        if (h.xs.get(0L) instanceof Lin) {\n"
        "        } else { return -3; }\n"
        "        if (h.at(0L) instanceof Lin && h.at(1L) instanceof Base) {\n"
        "        } else { return -4; }\n"
        "        return 1;\n")), 1);
}

// 3.2: the pattern binds the result and the call runs once.
TEST(InstanceofCallLhsTests, aPatternOnACallBindsItsResultOnce) {
    EXPECT_EQ(runI32(program(
        "        int32 got = 0;\n"
        "        if (h.next() instanceof Lin l) { got = l.tag; }\n"
        "        if (h.calls != 1) { return -1; }\n"
        "        return got;\n")), 7);
}

// 3.3: interface- and Object-typed call results ask the runtime too.
TEST(InstanceofCallLhsTests, interfaceAndObjectCallResultsAskTheRuntime) {
    EXPECT_EQ(runI32(program(
        "        if (h.shape() instanceof Lin) {\n"
        "        } else { return -1; }\n"
        "        if (h.any(0L) instanceof Lin) {\n"
        "        } else { return -2; }\n"
        "        if (h.any(1L) instanceof Lin) { return -3; }\n"
        "        return 1;\n")), 1);
}

// 3.4: a call already of the target type stays true.
TEST(InstanceofCallLhsTests, aCallOfTheTargetTypeStaysTrue) {
    EXPECT_EQ(runI32(program(
        "        if (h.lin() instanceof Lin) { return 1; }\n"
        "        return -1;\n")), 1);
}
