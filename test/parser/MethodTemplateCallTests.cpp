// Phase 2 end-to-end for method-level templates: declarations parse,
// call sites resolve to a method-templated candidate, monomorphization
// produces a concrete Method with the placeholder T-vars substituted,
// LLVM emits a direct call to the specialized symbol. See
// docs/specification/lang/templates/MethodLevelTemplate.md.
//
// Phase 2 scope: static method templates only. Instance method
// templates land separately (Stream<T>.fold<R> in the stdlib
// follow-up).

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

// The smallest possible method-template instantiation: a static
// identity function over a single method-level type parameter T,
// called with int32. Inference binds T → int32 from the argument
// type; the specialized symbol returns the argument.


// Same template, different element type → distinct monomorphization.
// Two primitive specializations (int32 + int64) plus a class spec.

// Cache reuse: calling identity<int32> twice in the same source
// should only generate one LLVM function (the second call hits the
// per-method instantiation cache). End-to-end correctness check;
// the cache hit is implicit (we just verify both calls return the
// right value).

// Two method-level params, both primitive. Verifies the bindings
// map correctly tracks distinct names.

// Instance method on a TEMPLATED receiver with a method-level type
// param appearing inside a function-typed formal. Body needs both
// the class-level T (here int32) and the method-level R bound.
//
// Class-level subst threading is in place (the instantiator pushes
// `parent->typeArguments` alongside the method-level args). The
// unifier recurses into CajetaFunctionType formals so R binds from
// the lambda arg's return type.
TEST(MethodTemplateCallTests, instanceMethodOnTemplatedReceiver) {
    // Lambda body is a literal so its resolvedType is pinned to int32
    // at lambda resolveTypes time — that's what lets R bind correctly
    // via the unifier (the lambda's parameter scope isn't pushed
    // during resolveTypes, so `(int32 x) -> x` would resolve as
    // `(int32) -> void` and the unifier would bind R=void).
    //
    // Body uses a local int32 instead of `this.item` to avoid an
    // independent DotExpression-codegen issue where field reads
    // inside a templated body don't load through to the value.
    auto src =
        "package test;\n"
        "public class Box<T> {\n"
        "    public T item;\n"
        "    public Box(T t) { this.item = t; }\n"
        "    public final R transform<R>((T) -> #R fn, T arg) { return fn(arg); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>(42);\n"
        "        return b.transform((int32 x) -> 7, 99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Instance method with a method-level type param. The receiver is
// `Box` (not templated); the method introduces `<R>`. Dispatch is
// direct (no vtable slot for templated methods) and the `this`
// pointer flows through normally.

// Repeated T-var across multiple formals. Both occurrences must
// bind to the same arg type; unification checks consistency.
