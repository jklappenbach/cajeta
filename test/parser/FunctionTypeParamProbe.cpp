// P6.5 — function type as method parameter (grammar fix).
//
// Pre-P6.5: `(int32) -> void` failed to parse with "no viable
// alternative" because functionType's return slot was `typeType`
// (which doesn't include `void`). After the grammar fix the return
// is `typeTypeOrVoid`, so `(T) -> void fn` is a legal method
// parameter shape. CajetaType::fromContext was updated in lockstep
// to walk fnt->typeType() for params and fnt->typeTypeOrVoid() for
// the return (the old code lumped them all into typeType()).
//
// These probes cover parse + class-declaration shape. Calling
// through the function-typed parameter from a passed lambda /
// method-ref is still on the codegen path (the local-variable
// equivalent works via the L2 closure ABI; parameter-positioned
// function-typed slots need the same wiring extended) — pinned in
// the parameter probes here for the parse + declaration step.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

using cajeta_test::CajetaJit;

TEST(FunctionTypeParamProbe, parseOnlyConcreteFnTypeAsParam) {
    // Smallest probe: does the formalParameter shape just *parse*?
    // The original P6.5 note said it failed with "no viable
    // alternative at input '(int32) -> void'". Probe verifies the
    // grammar accepts it today, then we move to codegen.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static void apply((int32) -> void fn) {\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}


TEST(FunctionTypeParamProbe, lambdaLiteralDirectlyAsMethodArg) {
    // The MCE arg-eval loop now propagates expectedType to lambda
    // arguments from the target method's declared parameter types
    // (when a single-arity-matching same-name candidate exists).
    // Lets `apply((int32 v) -> ..., 41)` work without binding to a
    // typed local first.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 apply((int32) -> int32 fn, int32 x) {\n"
        "        return fn(x);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return apply((int32 v) -> { return v + 1; }, 41);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(FunctionTypeParamProbe, fnTypeOnGenericClassMethod) {
    // The shape that originally motivated the fix: generic stdlib
    // class with a method that accepts a function-typed parameter
    // referencing the class's type parameter (Stream.forEach,
    // Optional.ifPresent, etc.). Concrete-type version here; generic
    // version is exercised in cajeta.lang.stream.Stream.forEach once it
    // lands as part of P6.5 stream combinators.
    auto src =
        "package test;\n"
        "public final class S {\n"
        "    public static int32 lastResult;\n"
        "    public static void apply((int32) -> void fn) {\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}
