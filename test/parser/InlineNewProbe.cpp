//
// Regression tests for `heap T(...)` used inline as a method-call
// argument. Previously the call's parameter-eval loop fell back to
// `CajetaType::of(value)` because NewExpression didn't set its own
// resolvedType during the resolve-pass — `of(value)` returns the
// generic `pointer` type rather than the actual class, so
// resolveMethod couldn't match the call, invokeMethod returned null,
// and ReturnStatement segfaulted on `val->getType()` of a null val.
// Fix: NewExpression now overrides resolveTypes and sets
// resolvedType from the type-name lookup (mirroring what
// generateCode was already doing internally).
//

#include "gtest/gtest.h"
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

} // namespace

// Minimal repro: `consume(heap Payload())` returned from run().
// Without the resolveTypes override this segfaulted during codegen
// of the return statement (null-typed value flowed through invoke
// failure and into ReturnStatement::generateCode at val->getType()).
TEST(InlineNewProbe, basic) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 consume(Payload p) { return 7; }\n"
        "    public static int32 run() {\n"
        "        return consume(heap Payload());\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Multi-argument shape: inline-new mixed with primitives. Exercises
// the resolveMethod canonical-signature match across heterogeneous
// argument types where one is a fresh class allocation.
TEST(InlineNewProbe, mixedInlineNewAndPrimitive) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 consume(Payload p, int32 extra) { return extra; }\n"
        "    public static int32 run() {\n"
        "        return consume(heap Payload(), 17);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 17);
}

// Statement-form use of an inline-new call. The non-return path
// covers MethodCallExpression's normal codegen continuation rather
// than going through ReturnStatement, so the failure mode would
// look different here if the fix were narrow to ReturnStatement.
TEST(InlineNewProbe, inlineNewAsStatementCall) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 n;\n"
        "    public Payload() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 consume(Payload p) { return 0; }\n"
        "    public static int32 run() {\n"
        "        consume(heap Payload());\n"
        "        return 99;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}
