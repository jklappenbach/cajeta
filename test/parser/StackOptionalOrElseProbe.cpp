//
// #56: SIGSEGV reported as "third sequential stack `Optional<int32>`
// local whose terminal is `.orElse(literal)` crashes" — narrowed via
// bisection to a different root cause than the original sret-frame
// hypothesis:
//
// `orElse(-1)` passes a `PrefixExpression(PREFIX_OP_NEGATIVE,
// IntegerLiteral(1))`. PrefixExpression had no `resolveTypes` override,
// so the base recursion resolved the integer-literal child but never
// set the prefix expression's OWN resolvedType. `getResolvedType()`
// returned null.
//
// The crash path was `LocalVariableDeclaration::generateCode` doing
// receiver-class pre-resolution to decide `initIsStackAlloc` /
// `initIsBorrow`: it built a `ParameterEntry` list off the call's
// expressions and passed it to `CajetaClass::resolveMethod`, whose
// `Method::buildGeneric` dereferenced `parameter.type->toGeneric()`
// on a null type pointer.
//
// Why the "multiple Optionals" framing happened to surface it: the
// LVD pre-resolution path only runs when the RHS is a method call
// AND there's an initializer-side decision to make for the new local.
// A single `return oa.orElse(-1)` goes through ReturnExpression, not
// LVD — and the regular `MethodCallExpression::generateCode` path
// falls back to `CajetaType::of(value)` on the post-codegen LLVM
// value, so the missing resolvedType was silently recovered.
//
// Positive literals (`oa.orElse(99)`) work because `IntegerLiteral`
// has its own `resolveTypes` that sets its type — no PrefixExpression
// wrapping.
//
// Fix: `PrefixExpression::resolveTypes` resolves children, then sets
// its own resolvedType to the operand's type (or `boolean` for `!`).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    auto src =
        "package test;\n"
        "import cajeta.threading.Channel;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        + body +
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int32_t runI32Direct(const std::string& body) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        + body +
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Baseline: two sequential locals, third terminal is isPresent/get.
// Already pinned by ChannelTests.sequentialReceivesIsPresent — repeated
// here for local diff against the failing variant.
TEST(StackOptionalOrElseProbe, threeLocalsThirdIsPresentGet) {
    int32_t v = runI32(
        "    public static int32 run() {\n"
        "        Channel<int32> ch = new Channel<int32>(2);\n"
        "        ch.send(11);\n"
        "        ch.send(22);\n"
        "        ch.close();\n"
        "        Optional<int32> oa = ch.receive();\n"
        "        int32 a = oa.get();\n"
        "        Optional<int32> ob = ch.receive();\n"
        "        int32 b = ob.get();\n"
        "        Optional<int32> oc = ch.receive();\n"
        "        int32 c = -1;\n"
        "        if (oc.isPresent()) { c = oc.get(); }\n"
        "        return a + b + c;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 32);  // 11 + 22 + (-1)
}

// Even smaller: one stack Optional<int32> with orElse(literal).
TEST(StackOptionalOrElseProbe, singleStackOptionalOrElseLiteral) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(false, 0);\n"
        "        return oa.orElse(-1);\n"
        "    }\n"
    );
    EXPECT_EQ(v, -1);
}

// Two stack Optionals — first with .get(), second with .orElse(literal).
TEST(StackOptionalOrElseProbe, twoStackOptionalsSecondOrElseLiteral) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(true, 11);\n"
        "        int32 a = oa.get();\n"
        "        Optional<int32> ob = stack Optional<int32>(false, 0);\n"
        "        int32 b = ob.orElse(-1);\n"
        "        return a + b;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 10);  // 11 + (-1)
}

// Same shape as bothOrElseLiteral but the second orElse arg is a
// POSITIVE literal — bisects whether `-1` (unary minus) is the trigger
// vs the structural "two Optionals + orElse" frame layout.
TEST(StackOptionalOrElseProbe, twoStackOptionalsBothOrElsePositiveLiteral) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(true, 11);\n"
        "        int32 a = oa.orElse(99);\n"
        "        Optional<int32> ob = stack Optional<int32>(false, 0);\n"
        "        int32 b = ob.orElse(99);\n"
        "        return a + b;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 110);  // 11 + 99
}

// Two stack Optionals, both use .orElse(literal).
TEST(StackOptionalOrElseProbe, twoStackOptionalsBothOrElseLiteral) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(true, 11);\n"
        "        int32 a = oa.orElse(-1);\n"
        "        Optional<int32> ob = stack Optional<int32>(false, 0);\n"
        "        int32 b = ob.orElse(-1);\n"
        "        return a + b;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 10);  // 11 + (-1)
}

// Two stack Optionals, first uses .orElse(literal), second uses .get().
TEST(StackOptionalOrElseProbe, twoStackOptionalsFirstOrElseSecondGet) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(false, 0);\n"
        "        int32 a = oa.orElse(-1);\n"
        "        Optional<int32> ob = stack Optional<int32>(true, 22);\n"
        "        int32 b = ob.get();\n"
        "        return a + b;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 21);  // -1 + 22
}

// Direct stack-construction of three Optional<int32>, third uses orElse.
// Isolates the bug from the Channel<T> stack-Optional return path.
TEST(StackOptionalOrElseProbe, threeStackOptionalsThirdOrElseLiteral) {
    int32_t v = runI32Direct(
        "    public static int32 run() {\n"
        "        Optional<int32> oa = stack Optional<int32>(true, 11);\n"
        "        int32 a = oa.get();\n"
        "        Optional<int32> ob = stack Optional<int32>(true, 22);\n"
        "        int32 b = ob.get();\n"
        "        Optional<int32> oc = stack Optional<int32>(false, 0);\n"
        "        int32 c = oc.orElse(-1);\n"
        "        return a + b + c;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 32);
}

// The bug shape: third sequential local + orElse(literal).
TEST(StackOptionalOrElseProbe, threeLocalsThirdOrElseLiteral) {
    int32_t v = runI32(
        "    public static int32 run() {\n"
        "        Channel<int32> ch = new Channel<int32>(2);\n"
        "        ch.send(11);\n"
        "        ch.send(22);\n"
        "        ch.close();\n"
        "        Optional<int32> oa = ch.receive();\n"
        "        int32 a = oa.get();\n"
        "        Optional<int32> ob = ch.receive();\n"
        "        int32 b = ob.get();\n"
        "        Optional<int32> oc = ch.receive();\n"
        "        int32 c = oc.orElse(-1);\n"
        "        return a + b + c;\n"
        "    }\n"
    );
    EXPECT_EQ(v, 32);  // 11 + 22 + (-1)
}
