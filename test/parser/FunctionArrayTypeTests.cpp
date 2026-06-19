//
// Array-of-function-types — `((T)->R)[]`. The grouping parens make an array OF
// a function type expressible (without them, `(T)->R[]` binds the `[]` to the
// RETURN). Grammar: `typeType : ... | '(' functionType ')' ('[' ']')*`;
// CajetaType::fromContext folds the bracket-wrapping into the functionType
// branch so the element is a CajetaArray over a CajetaFunctionType.
//
// On host this is a real dispatch table: the array stores closure-record
// pointers (the `{fn,captures,drop}` ABI), `ops[i]` loads one element, and
// `ops[i](args)` (CallExpression) lowers to an indirect call through the shared
// closure ABI — the same path as the bare `op(args)` form. See
// docs/specification/lang/Lambdas.md. The device-bounded form (tag + if/else over a
// statically-known candidate set) rides on the same surface.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kOps =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 sq(int32 x)   { return x * x; }\n"
    "    public static int32 cube(int32 x) { return x * x * x; }\n"
    "    public static int32 neg(int32 x)  { return 0 - x; }\n";

} // namespace

// Parse + type resolution + array build: a literal table of method references
// resolves to CajetaArray<fn> and reports its element count(). Proves the
// grammar/type path end-to-end without yet calling through an element.
TEST(FunctionArrayTypeTests, buildsTableAndCounts) {
    std::string src = std::string(kOps) +
        "    public static int32 run() {\n"
        "        ((int32) -> int32)[] ops = { D::sq, D::cube, D::neg };\n"
        "        return ops.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// Indexed dispatch with constant indices: `ops[0]` is sq, `ops[1]` is cube.
// 5*5 + 3*3*3 = 25 + 27 = 52. Proves `ops[i](args)` lowers to an indirect
// call through the selected closure.
TEST(FunctionArrayTypeTests, indexedConstantDispatch) {
    std::string src = std::string(kOps) +
        "    public static int32 run() {\n"
        "        ((int32) -> int32)[] ops = { D::sq, D::cube, D::neg };\n"
        "        return ops[0](5) + ops[1](3);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 52);
}

// Genuine runtime dispatch: the index is a loop variable, so the candidate is
// chosen at runtime (a constant-folded single candidate would fail). Sum over
// i in 0..2 of ops[i](i+2): sq(2)=4, cube(3)=27, neg(4)=-4 -> 27.
TEST(FunctionArrayTypeTests, runtimeIndexDispatch) {
    std::string src = std::string(kOps) +
        "    public static int32 run() {\n"
        "        ((int32) -> int32)[] ops = { D::sq, D::cube, D::neg };\n"
        "        int32 acc = 0;\n"
        "        for (int32 i = 0; i < 3; i = i + 1) {\n"
        "            acc = acc + ops[i](i + 2);\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 27);
}
