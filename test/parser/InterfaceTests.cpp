//
// Interface behavioral tests. v1 scope:
//   - declare an interface with abstract method signatures
//   - a class implements an interface and provides matching methods
//   - dispatch through an interface-typed variable lands on the class's impl
//   - multiple classes implementing the same interface dispatch independently
//
// Not in v1 scope (separately tracked in GAP-1 follow-ups):
//   - default methods (interface methods with bodies)
//   - interface-extends-interface
//   - interface fields / constants
//   - compile-time check that a class actually implements every interface method
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

// Simplest case: interface + one implementing class + direct call on the
// concrete class. The class's own dispatch path should not be disturbed
// by the new interface machinery.
TEST(InterfaceTests, classWithInterfaceStillDispatchesDirectly) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Hello h = heap Hello();\n"
        "        return h.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// The crown jewel: dispatch through the *interface* type lands on the
// implementing class's method. The class's vtable carries an entry keyed
// by the interface method's hash, pointing to the class's concrete fn.
TEST(InterfaceTests, dispatchThroughInterfaceVariableLandsOnImpl) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Hello implements Greeter {\n"
        "    public int32 greet() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Greeter g = heap Hello();\n"
        "        return g.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Two unrelated classes implement the same interface. A variable typed
// as the interface dispatches to each class's own implementation in turn.
TEST(InterfaceTests, twoImplementersDispatchIndependently) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public class Aye implements Greeter {\n"
        "    public int32 greet() { return 3; }\n"
        "}\n"
        "public class Bee implements Greeter {\n"
        "    public int32 greet() { return 5; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Greeter a = heap Aye();\n"
        "        Greeter b = heap Bee();\n"
        "        return a.greet() + b.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// ---------------------------------------------------------------------------
// iface-generic-returns (specs/iface-generic-returns-spec.md, fixed
// 2026-07-30). Interface decls used to FORCE the sret ABI for every class
// return except literal cajeta.lang.String (Method.cpp returnsStackValue
// #63), while impls returning ordinary heap classes emit `ret ptr` — the
// indirect call misaligned (sret slot became `this`) and dispatch
// SIGSEGV'd. The fix flips the exception to a whitelist: sret only for the
// value-shape convention (Optional); reference ABI otherwise. These pin
// every convention.
// ---------------------------------------------------------------------------

// A generic-class return (Tensor<float64>) through an interface-typed
// receiver — the cajeta-ml Predictor.predict shape. Plain borrow return.
TEST(InterfaceTests, genericClassReturnDispatchesThroughInterface) {
    auto src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public interface Holder {\n"
        "    Tensor<float64> peek();\n"
        "}\n"
        "public final class Cell implements Holder {\n"
        "    private Tensor<float64> held;\n"
        "    public Cell(Tensor<float64> t) {\n"
        "        this.held #= t.copy();\n"
        "        return;\n"
        "    }\n"
        "    public Tensor<float64> peek() { return this.held; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] dy = [ 1.0, 3.0, 5.0 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float64> y #= Tensor.of<float64>(dy, s3);\n"
        "        Holder h = heap Cell(y);\n"
        "        Tensor<float64> p = h.peek();\n"
        "        if (p.get1(1) != 3.0) { return 1; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// An OWNED generic-class return (#Tensor<float64>) through the interface —
// ownership transfer across interface dispatch.
TEST(InterfaceTests, ownedGenericReturnDispatchesThroughInterface) {
    auto src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public interface Maker {\n"
        "    #Tensor<float64> make(int64 n);\n"
        "}\n"
        "public final class Filler implements Maker {\n"
        "    public Filler() { return; }\n"
        "    public #Tensor<float64> make(int64 n) {\n"
        "        int64[] sh = heap int64[1]; sh[0] = n;\n"
        "        Tensor<float64> t #= Tensor.full<float64>(sh, 7.0);\n"
        "        return #t;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Maker m = heap Filler();\n"
        "        Tensor<float64> t #= m.make((int64) 4);\n"
        "        if (t.shapeAt(0) != 4) { return 1; }\n"
        "        if (t.get1(3) != 7.0) { return 2; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// String return through an interface — the reference-ABI convention the old
// code special-cased (the ifx Backend.name() shape). Must keep working.
TEST(InterfaceTests, stringReturnDispatchesThroughInterface) {
    auto src =
        "package test;\n"
        "public interface Named {\n"
        "    #String name();\n"
        "}\n"
        "public final class Thing implements Named {\n"
        "    public Thing() { return; }\n"
        "    public #String name() { return \"thing\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Named n = heap Thing();\n"
        "        String s #= n.name();\n"
        "        if (s.count() != 5) { return 1; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Optional<T> return through an interface — the value-shape sret convention
// the whitelist PRESERVES (the AsyncIterator.next() shape). Impl returns
// `stack Optional`; decl and impl must both stay sret.
TEST(InterfaceTests, optionalReturnKeepsSretConvention) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public interface Source {\n"
        "    Optional<int32> next();\n"
        "}\n"
        "public final class Once implements Source {\n"
        "    public Once() { return; }\n"
        "    public Optional<int32> next() {\n"
        "        return stack Optional<int32>(true, 9);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Source s = heap Once();\n"
        "        Optional<int32> o = s.next();\n"
        "        if (!o.isPresent()) { return 1; }\n"
        "        if (o.get() != 9) { return 2; }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
