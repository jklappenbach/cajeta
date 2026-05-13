//
// L4 lambda tests — method references.
//
// L4-1 (this file's initial slice): static method references of the
// form `Type::staticMethod`. The reference compiles to a closure-typed
// value pointing at a synthesized thunk that forwards to the underlying
// static method, so callers see the same shape as a non-capturing
// lambda. Stored, passed, and invoked the same way.
//
// Out of scope (later L4 sub-slices):
//   - L4-2: bound instance method references (`obj::method`)
//   - L4-3: unbound instance method references (`Type::instanceMethod`)
//   - L4-4: constructor references (`Type::new`)
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

} // namespace

// Basic shape: declare a static method on a helper class, take a
// reference to it, store in a function-typed local, invoke.
TEST(LambdaL4Tests, staticMethodReferenceCallable) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 doubled(int32 x) { return x * 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 fn = Util::doubled;\n"
        "        return fn(7);\n"  // 14
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 14);
}

// Multi-argument static method — verifies the thunk forwards every
// declared param to the underlying method in order.
TEST(LambdaL4Tests, staticMethodReferenceMultiArg) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 combine(int32 a, int32 b) {\n"
        "        return a * 10 + b;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32, int32) -> int32 fn = Util::combine;\n"
        "        return fn(3, 4);\n"  // 34
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 34);
}

// Same static-method ref taken twice in the same function. Should
// resolve to the same thunk (or at least produce identical behaviour
// at both call sites).
TEST(LambdaL4Tests, staticMethodReferenceTwiceConsistent) {
    auto src =
        "package test;\n"
        "public class Util {\n"
        "    public static int32 inc(int32 x) { return x + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 a = Util::inc;\n"
        "        (int32) -> int32 b = Util::inc;\n"
        "        return a(10) + b(20);\n"  // 11 + 21 = 32
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 32);
}

// The other method-reference forms still throw NOT_IMPLEMENTED at
// codegen so the parser doesn't reject them outright — they're
// scheduled for L4-2/L4-3/L4-4. The error mentions the specific shape
// so callers know what's missing.
TEST(LambdaL4Tests, constructorRefStillNotImplemented) {
    // No explicit ctor — the constructor-ref doesn't actually invoke
    // anything in this test; codegen rejects the form before reaching
    // the underlying ctor.
    auto src =
        "package test;\n"
        "public class Foo { }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> Foo ctor = Foo::new;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected constructor-ref NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NOT_IMPLEMENTED");
        EXPECT_NE(e.getMessage().find("constructor reference"), std::string::npos);
    }
}

// ---------------------------------------------------------------------
// L4-2: bound instance method references — `obj::method`
// ---------------------------------------------------------------------

// Receiver captured by borrow; thunk loads the captured `this` and
// dispatches the instance method with it.
TEST(LambdaL4Tests, boundInstanceMethodReferenceCallable) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 next() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = new Counter();\n"
        "        () -> int32 fn = c::next;\n"
        "        return fn();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// L4-3: unbound instance method references — `Type::instanceMethod`
// ---------------------------------------------------------------------

// The function-value type has the receiver as its first explicit
// parameter; the caller passes a receiver at the call site and the
// thunk forwards it as `this`. Non-capturing — closure record is a
// private global, no escape concerns.
TEST(LambdaL4Tests, unboundInstanceMethodReferenceCallable) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 next() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = new Counter();\n"
        "        (Counter) -> int32 fn = Counter::next;\n"
        "        return fn(c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// Multi-arg unbound instance ref: receiver + extra method params.
TEST(LambdaL4Tests, unboundInstanceMethodReferenceMultiArg) {
    auto src =
        "package test;\n"
        "public class Adder {\n"
        "    public int32 plus(int32 x) { return x + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Adder a = new Adder();\n"
        "        (Adder, int32) -> int32 fn = Adder::plus;\n"
        "        return fn(a, 10);\n"  // 11
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// Unbound refs don't capture — safe to return from a producing method
// since the closure record is a private global constant.
TEST(LambdaL4Tests, returnedUnboundInstanceRefCallable) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 next() { return 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static (Counter) -> int32 mkFn() {\n"
        "        return Counter::next;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        (Counter) -> int32 fn = mkFn();\n"
        "        Counter c = new Counter();\n"
        "        return fn(c);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Returning a bound-instance ref must be rejected by the L3-2 escape
// check — the captured receiver is borrowed and would dangle past the
// producing method's return.
TEST(LambdaL4Tests, returnBoundInstanceRefIsBorrowEscape) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 next() { return 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static () -> int32 mkFn() {\n"
        "        Counter c = new Counter();\n"
        "        () -> int32 fn = c::next;\n"
        "        return fn;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected bound-instance-ref to be rejected on escape";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_BORROW_ESCAPE");
    }
}
