// Regression for explicit-method-type-arg return typing. A templated call
// (`foo<T>(...)`) over a same-arity non-template overload must pin its static
// return type to the instantiated `T`, not the non-template overload's return.
// Json.parse is the canonical case: parse<T>(int8[],int64)->T coexists with
// parse(int8[],int64)->JsonValue, so `Json.parse<Box>(...)` must type as Box.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// Chaining a member directly off the call result forces the call's *static*
// type to be correct: `.id` only resolves if parse<Box> types as Box (not the
// same-arity parse->JsonValue overload, which has no `id`).
TEST(MethodTemplateReturnTypeTests, explicitArgPinsReturnTypeForChaining) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box { public int32 id; }\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.id = 42;\n"
        "        int8[] b #= Json.toBytes<Box>(a);\n"
        "        return Json.parse<Box>(b, (int64) b.count()).id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// And assigning the result to a local of the instantiated type is well-typed.
TEST(MethodTemplateReturnTypeTests, explicitArgResultAssignsToInstantiatedType) {
    auto src =
        "package test;\n"
        "import cajeta.codec.json.Json;\n"
        "public class Box { public int32 id; }\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box a = heap Box();\n"
        "        a.id = 7;\n"
        "        int8[] b #= Json.toBytes<Box>(a);\n"
        "        Box c #= Json.parse<Box>(b, (int64) b.count());\n"
        "        return c.id;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
