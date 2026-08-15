// A PRIMITIVE crossing a WILDCARD slot — `(float64) e` on a `T`-typed value,
// and `(T) someFloat64` back the other way.
//
// A `?`-typed slot is one pointer-sized word holding the value's BITS inline,
// so both directions are bit REINTERPRETATIONS. `CastExpression::generateCode`
// reached its fallback `bitcast` for them, and LLVM rejects a bitcast with a
// pointer on exactly one side:
//
//     Invalid bitcast   %747 = bitcast double %746 to ptr
//     Invalid bitcast   %29  = bitcast ptr %28 to double
//
// The concrete instantiation never shows it — there `T` IS float64 and the
// cast is a no-op. Only the WILDCARD instantiation's bodies carry the bad IR,
// which is why this went unnoticed: nothing calls `Foo<?>::bar` at runtime, so
// the damage is confined to whether the module VERIFIES. It does not, and a
// module that does not verify takes every module referencing it down with it.
//
// Found via jupyter-kernel 7.2.5: `dev.cajeta.ml.grad.GradTape<E>` has
// `una(int32 op, GradTensor a, int32 axis, E c)` calling
// `push(..., (float64) c, ...)`. Its `<?>` instantiation was the one module in
// a 144-module archive that failed to verify, and the ~40 modules that
// reference GradTape then failed to materialize.
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

// The generic under test, verbatim in shape from GradTape: a payload array of
// a CONCRETE primitive type, written from a `T` and read back into one. Both
// casts are no-ops in the concrete instantiation and pointer/double hops in
// the wildcard one.
const char* kTape =
    "public class Tape<T> {\n"
    "    float64[] cs;\n"
    "    public Tape() { this.cs = heap float64[2]; }\n"
    // `note` rather than `record`: the latter is a type keyword.
    "    public int32 note(T c) {\n"
    "        this.cs[0] = (float64) c;\n"
    "        return 1;\n"
    "    }\n"
    "    public T replay() { return (T) this.cs[0]; }\n"
    "}\n";

}  // namespace

// The compile must SUCCEED — which is the whole assertion, because
// JitTestHelper runs `llvm::verifyModule` before it hands anything to the JIT.
// Declaring the wildcard-typed parameter is what forces `Tape<?>`'s bodies to
// be emitted alongside the concrete instantiation's.
TEST(WildcardScalarCastTests, wildcardInstantiationOfAScalarCastVerifies) {
    std::string src = std::string("package test;\n") + kTape +
        "public final class D {\n"
        "    public static int32 depth(Tape<?> w) { return 7; }\n"
        "    public static int32 run() {\n"
        "        Tape<float64> t = heap Tape<float64>();\n"
        "        t.note(2.5);\n"
        "        return (int32) t.replay();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// float32 through a 64-bit slot. The reinterpretation is a zext/trunc on the
// way through the pointer word, not a bitcast — sizing the hop to the operand
// instead of the pointer would produce IR that verifies and silently reads the
// wrong half.
TEST(WildcardScalarCastTests, narrowerFloatThroughAPointerSlotVerifies) {
    std::string src =
        "package test;\n"
        "public class Cell<T> {\n"
        "    float32[] xs;\n"
        "    public Cell() { this.xs = heap float32[2]; }\n"
        "    public int32 put(T c) { this.xs[0] = (float32) c; return 1; }\n"
        "    public T get() { return (T) this.xs[0]; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 depth(Cell<?> w) { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cell<float32> c = heap Cell<float32>();\n"
        "        c.put(3.5f);\n"
        "        return (int32) c.get();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}
